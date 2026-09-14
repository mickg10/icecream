#!/usr/bin/env python3
"""Fail-closed authority for a literal root-header Firefox A/B corpus.

This module is intentionally separate from ``firefox_corpus_authority``.  The
older authority mutates preprocessed ``.ii`` files directly; this authority
requires a recorded true-path header bind and independently checks that every
retained B body is exactly A plus the one header-edit byte.
"""

from __future__ import annotations

import argparse
import ctypes
from concurrent.futures import ThreadPoolExecutor, as_completed
import errno
import hashlib
import json
import mmap
import os
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Mapping

from .schema_validation import ValidationError, canonical_bytes, load_json


AUTHORITY_SCHEMA = "icefarm-firefox-root-header-authority-v1"
TRACE_SCHEMA = "icefarm-firefox-root-selection-v1"
PAIR_SCHEMA = "icefarm-firefox-root-pair-index-v1"
COMPILE_SCHEMA = "icefarm-firefox-root-compile-validation-v1"
RETAINED_EXECUTION_SCHEMA = "icefarm-firefox-root-retained-execution-v1"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
TRUE_PATH_MODES = frozenset(("true-path-bind", "retained-true-path-revalidation"))
NAMESPACE_ARGV = ("unshare", "-Urm", "--propagation", "unchanged")
NAMESPACE_TIMEOUT_ARGV = (*NAMESPACE_ARGV, "--pid", "--fork", "--kill-child")
RETAINED_ROW_FIELDS = frozenset(
    {
        "actual_input", "argv", "command_sha256", "compiler_path", "compiler_sha256", "elapsed_ms",
        "bind_device", "bind_inode", "bind_source_sha256", "bind_target_sha256", "exit_code", "index", "input_sha256",
        "mount_argv", "mount_executable", "mount_executable_sha256", "mountinfo_path", "mountinfo_row_sha256",
        "mountinfo_sha256", "namespace_argv",
        "reproduced_sha256", "resolution_basis", "resolution_row_sha256", "retained_path",
        "retained_sha256", "stderr_first_diagnostic", "stderr_path", "stderr_sha256", "stdout_sha256", "timed_out",
        "turn", "unmount_argv", "umount_executable", "umount_executable_sha256", "working_directory",
        "equal", "error", "source_device", "source_inode", "target_device", "target_inode",
    }
)
RETAINED_DOC_FIELDS = frozenset(
    {"compile_commands_sha256", "count", "executor", "rows", "schema", "source_error", "source_evidence"}
)
RETAINED_EXECUTOR_FIELDS = frozenset({"jobs", "timeout_s"})
CAPTURE_SCHEMA = "icefarm-firefox-root-header-capture-v1"


class RootHeaderAuthorityError(ValueError):
    """The root-header authority is incomplete, ambiguous, or tampered."""


def _fail(subject: str, message: str) -> None:
    raise RootHeaderAuthorityError(f"{subject}: {message}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _sha(value: Any, subject: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        _fail(subject, "must be a lowercase SHA-256 digest")
    return value


def _mapping(value: Any, subject: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        _fail(subject, "must be an object")
    return value


def _safe_output(value: Any, subject: str) -> Path:
    if not isinstance(value, str):
        _fail(subject, "must be an absolute non-symlink output path")
    parsed = PurePosixPath(value)
    if (
        not parsed.is_absolute()
        or value == "/"
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a safe absolute path")
    path = Path(value)
    if path.is_symlink() or path.exists() and not path.is_file():
        _fail(subject, "must not name a symlink or non-file")
    return path


def _compiler(value: str, subject: str) -> tuple[str, str]:
    candidate = Path(value) if os.path.isabs(value) else Path(shutil.which(value) or "")
    try:
        resolved = candidate.resolve(strict=True)
    except OSError:
        resolved = Path()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        _fail(subject, "compiler executable is not an executable regular file")
    return str(resolved), _sha256(resolved)


def _command_argv(raw: Mapping[str, Any], subject: str) -> list[str]:
    arguments = raw.get("arguments")
    command = raw.get("command")
    if arguments is not None and command is not None:
        _fail(subject, "must contain exactly one of arguments or command")
    if arguments is not None:
        if not isinstance(arguments, list) or not arguments or not all(
            isinstance(item, str) and item for item in arguments
        ):
            _fail(subject, "arguments must be a nonempty string array")
        return list(arguments)
    if not isinstance(command, str) or not command:
        _fail(subject, "must contain arguments or command")
    try:
        argv = shlex.split(command, posix=True)
    except ValueError as exc:
        _fail(subject, f"command cannot be parsed: {exc}")
    if not argv:
        _fail(subject, "command parses to an empty argv")
    return argv


def _preprocess_argv(candidate: Mapping[str, Any], input_path: Path) -> list[str]:
    argv = list(candidate["argv"])
    position = candidate["input_position"]
    if argv[position] != candidate.get("input_operand", argv[position]):
        _fail("compile_command", "input operand changed")
    # Preserve the compile database's exact spelling when preprocessing its
    # original input.  Clang writes that spelling into the leading linemarker,
    # so normalizing a relative operand to an absolute path changes otherwise
    # identical retained .ii bytes.  Substitution remains mandatory when the
    # caller intentionally supplies a different input.
    original = Path(candidate["actual_input"])
    if os.path.realpath(input_path) == os.path.realpath(original):
        argv[position] = candidate["input_operand"]
    else:
        argv[position] = str(input_path)
    argv[0] = candidate["compiler_path"]
    transformed: list[str] = []
    skip = False
    for index, token in enumerate(argv):
        if skip:
            skip = False
            continue
        if token == "-c":
            continue
        if token == "-o":
            skip = True
            continue
        transformed.append(token)
    if "-E" not in transformed:
        transformed.append("-E")
    return transformed


def _preprocess_matches(candidate: Mapping[str, Any], expected_a: Path, timeout_s: float = 120.0) -> bool:
    transformed = _preprocess_argv(candidate, Path(candidate["actual_input"]))
    try:
        result = subprocess.run(
            transformed,
            cwd=candidate["directory"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0 and result.stdout == expected_a.read_bytes()


def _command_specs(
    path: Path,
    trace_rows: list[dict[str, Any]],
    a_paths: tuple[Path, ...] | None = None,
    resolve_ambiguous: bool = False,
) -> dict[str, dict[str, Any]]:
    value = _json(path, "compile_commands")
    if not isinstance(value, list):
        _fail("compile_commands", "must be an array")
    requested = {row["actual_input"] for row in trace_rows}
    requested_paths = {actual: Path(os.path.realpath(actual)) for actual in requested}
    requested_by_resolved = {path: actual for actual, path in requested_paths.items()}
    candidates: dict[str, list[dict[str, Any]]] = {actual: [] for actual in requested}
    compiler_cache: dict[str, tuple[str, str]] = {}
    resolved_token_cache: dict[tuple[str, str], Path] = {}
    for index, raw in enumerate(value):
        row = _mapping(raw, f"compile_commands[{index}]")
        file_value = row.get("file")
        if file_value is not None and not isinstance(file_value, str):
            _fail(f"compile_commands[{index}].file", "must be a string when present")
        directory = row.get("directory")
        _directory(directory, f"compile_commands[{index}].directory")
        argv = _command_argv(row, f"compile_commands[{index}]")
        occurrences_by_actual: dict[str, list[int]] = {actual: [] for actual in requested}
        for position, token in enumerate(argv):
            if token.startswith("-"):
                continue
            if token in requested:
                actual = token
            else:
                cache_key = (directory, token)
                resolved = resolved_token_cache.get(cache_key)
                if resolved is None:
                    raw_path = os.fspath(Path(token) if os.path.isabs(token) else Path(directory) / token)
                    resolved = Path(os.path.realpath(raw_path))
                    resolved_token_cache[cache_key] = resolved
                actual = requested_by_resolved.get(resolved)
            if actual is not None:
                occurrences_by_actual[actual].append(position)
        for actual, occurrences in occurrences_by_actual.items():
            if not occurrences:
                continue
            if len(occurrences) != 1:
                _fail(f"compile_commands[{index}]", f"{actual} occurs more than once in argv")
            if argv[0] not in compiler_cache:
                compiler_cache[argv[0]] = _compiler(argv[0], f"compile_commands[{index}].compiler")
            compiler_path, compiler_sha256 = compiler_cache[argv[0]]
            candidates[actual].append(
                {
                    "actual_input": actual,
                    "argv": argv,
                    "command_sha256": hashlib.sha256(canonical_bytes(row)).hexdigest(),
                    "compiler_path": compiler_path,
                    "compiler_sha256": compiler_sha256,
                    "directory": directory,
                    "input_position": occurrences[0],
                    "input_operand": argv[occurrences[0]],
                    "file_exact": file_value == actual,
                }
            )
    specs: dict[str, dict[str, Any]] = {}
    for index, row in enumerate(trace_rows):
        actual = row["actual_input"]
        matches = candidates[actual]
        exact_matches = [candidate for candidate in matches if candidate["file_exact"]]
        basis = "exact-file" if exact_matches else "argv-operand"
        matches = exact_matches or matches
        if len(matches) > 1:
            if not resolve_ambiguous or a_paths is None:
                _fail(
                    f"selection.rows[{index}]",
                    "ambiguous compile commands require retained A preprocess evidence",
                )
            expected_a = a_paths[index]
            matches = [candidate for candidate in matches if _preprocess_matches(candidate, expected_a)]
            basis += "-preprocess-a"
        if len(matches) != 1:
            _fail(f"selection.rows[{index}]", f"expected one argv command for actual_input, got {len(matches)}")
        selected = matches[0]
        selected["resolution_basis"] = basis
        selected["resolution_row_sha256"] = selected["command_sha256"]
        specs[actual] = selected
    return specs


def _reconstruct_argv(spec: Mapping[str, Any], input_path: Path, output_path: Path) -> list[str]:
    argv = list(spec["argv"])
    position = spec["input_position"]
    if argv[position] != spec.get("input_operand", argv[position]):
        # The position is independently checked by _command_specs; this branch
        # is only a defensive guard for forged in-memory specs.
        _fail("compile_command", "input operand changed")
    argv[position] = str(input_path)
    output_positions = [index for index, token in enumerate(argv) if token == "-o"]
    if len(output_positions) > 1 or output_positions and output_positions[0] + 1 >= len(argv):
        _fail("compile_command", "must have at most one complete -o option")
    if output_positions:
        output_position = output_positions[0]
        argv[output_position + 1] = str(output_path)
    else:
        argv.extend(("-o", str(output_path)))
    argv[0] = str(spec["compiler_path"])
    return argv


def _git_snapshot(source_root: Path) -> dict[str, Any]:
    try:
        head_result = subprocess.run(
            ["git", "-C", str(source_root), "rev-parse", "--verify", "HEAD"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
        status_result = subprocess.run(
            ["git", "-C", str(source_root), "status", "--porcelain", "--untracked-files=all"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        _fail("source.git", f"cannot inspect worktree: {exc}")
    head = head_result.stdout.strip()
    status = status_result.stdout
    if head_result.returncode != 0 or status_result.returncode != 0:
        _fail("source.git", "cannot inspect worktree")
    return {
        "head": head,
        "status": status,
        "status_sha256": hashlib.sha256(status.encode()).hexdigest(),
    }


def _git_worktree(source_root: Path, expected_commit: str) -> dict[str, Any]:
    snapshot = _git_snapshot(source_root)
    if not HEX40_RE.fullmatch(snapshot["head"]):
        _fail("source.git.head", "source.root is not a Git worktree")
    if snapshot["head"] != expected_commit:
        _fail("source.git.head", f"expected {expected_commit}, got {snapshot['head']}")
    if snapshot["status"]:
        _fail("source.git.status", "source.root is not clean")
    return snapshot


def _source_header_snapshot(source_root: Path, source_commit: str, root_include: Path) -> dict[str, Any]:
    try:
        relative = root_include.relative_to(source_root)
    except ValueError:
        _fail("source.header", "root header is outside source.root")
    relative_text = relative.as_posix()
    try:
        tracked = subprocess.run(
            ["git", "-C", str(source_root), "ls-files", "--error-unmatch", "--", relative_text],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
        shown = subprocess.run(
            ["git", "-C", str(source_root), "show", f"{source_commit}:{relative_text}"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        _fail("source.header", f"cannot inspect tracked header: {exc}")
    current = root_include.read_bytes() if root_include.is_file() and not root_include.is_symlink() else b""
    return {
        "committed_sha256": hashlib.sha256(shown.stdout).hexdigest() if shown.returncode == 0 else None,
        "current_sha256": hashlib.sha256(current).hexdigest(),
        "path": str(root_include),
        "relative": relative_text,
        "tracked": tracked.returncode == 0 and tracked.stdout.strip() == relative_text,
    }


def _source_header_evidence(source_root: Path, source_commit: str, root_include: Path) -> dict[str, Any]:
    snapshot = _source_header_snapshot(source_root, source_commit, root_include)
    if not snapshot["tracked"]:
        _fail("source.header", "root header is not exactly tracked")
    if snapshot["committed_sha256"] != snapshot["current_sha256"]:
        _fail("source.header", "root header differs from committed bytes")
    return snapshot


def _regular(value: Any, subject: str) -> Path:
    if not isinstance(value, str):
        _fail(subject, "must be an absolute regular-file path")
    parsed = PurePosixPath(value)
    if (
        not parsed.is_absolute()
        or value == "/"
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a safe absolute path")
    path = Path(value)
    if not path.is_file() or path.is_symlink():
        _fail(subject, "must name a regular non-symlink file")
    return path


def _directory(value: Any, subject: str) -> Path:
    if not isinstance(value, str):
        _fail(subject, "must be an absolute directory path")
    parsed = PurePosixPath(value)
    if (
        not parsed.is_absolute()
        or value == "/"
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a safe absolute path")
    path = Path(value)
    if not path.is_dir() or path.is_symlink():
        _fail(subject, "must name a regular non-symlink directory")
    return path


def _json(path: Path, subject: str) -> Any:
    try:
        return load_json(path)
    except ValidationError as exc:
        _fail(subject, str(exc))


def _bound_file(value: Any, declared: Any, subject: str) -> Path:
    path = _regular(value, f"{subject}.path")
    expected = _sha(declared, f"{subject}.sha256")
    observed = _sha256(path)
    if observed != expected:
        _fail(subject, f"SHA-256 mismatch (expected {expected}, got {observed})")
    return path


def _write_once(path: Path, payload: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise RootHeaderAuthorityError(f"output already exists: {path}")
    path.write_bytes(payload)


def _safe_relative(value: Any, subject: str) -> str:
    if not isinstance(value, str):
        _fail(subject, "must be a normalized relative path")
    parsed = PurePosixPath(value)
    if (
        parsed.is_absolute()
        or str(parsed) != value
        or value in ("", ".")
        or ".." in parsed.parts
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        _fail(subject, "must be a normalized safe relative path")
    return value


def _manifest(path: Path, count: int, subject: str) -> tuple[Path, ...]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        _fail(subject, f"cannot read: {exc}")
    if len(lines) != count:
        _fail(subject, f"expected {count} rows, got {len(lines)}")
    result: list[Path] = []
    seen: set[Path] = set()
    for index, line in enumerate(lines):
        item = _regular(line, f"{subject}[{index}]")
        if item in seen:
            _fail(subject, f"duplicate path at index {index}")
        seen.add(item)
        result.append(item)
    return tuple(result)


def _manifest_root(paths: tuple[Path, ...], trace_rows: list[dict[str, Any]], subject: str) -> Path:
    relative = PurePosixPath(trace_rows[0]["ii_relative"])
    suffix = relative.parts
    first = paths[0].parts
    if len(first) <= len(suffix) or tuple(first[-len(suffix):]) != suffix:
        _fail(subject, "first manifest path does not contain the trace suffix")
    root = Path(*first[:-len(suffix)])
    for index, (path, trace) in enumerate(zip(paths, trace_rows, strict=True)):
        expected = root.joinpath(*PurePosixPath(trace["ii_relative"]).parts)
        if path != expected:
            _fail(f"{subject}[{index}]", "path is not exact root + trace relative path")
    return root


def _header_edit(before: Path, after: Path, diff: Path, root_include: str) -> dict[str, Any]:
    before_bytes = before.read_bytes()
    after_bytes = after.read_bytes()
    if before_bytes.count(b"\n") != after_bytes.count(b"\n"):
        _fail("root_edit", "header line count changed")
    if len(after_bytes) != len(before_bytes) + 1:
        _fail("root_edit", "edit must insert exactly one byte")
    prefix = 0
    while prefix < len(before_bytes) and before_bytes[prefix] == after_bytes[prefix]:
        prefix += 1
    if after_bytes[prefix + 1 :] != before_bytes[prefix:]:
        _fail("root_edit", "header is not an exact one-byte insertion")
    inserted = after_bytes[prefix]
    if inserted in (0, 10, 13):
        _fail("root_edit", "inserted byte must be a non-newline character")
    if not isinstance(root_include, str) or root_include != str(before):
        _fail("root_edit.path", "does not name the before-header path")
    diff_bytes = diff.read_bytes()
    diff_lines = diff_bytes.splitlines()
    line_number = before_bytes[:prefix].count(b"\n") + 1
    if len(diff_lines) == 3 and diff_lines[0] == f"{before}:{line_number}".encode():
        removed = [diff_lines[1][1:]] if diff_lines[1].startswith(b"-") else []
        added = [diff_lines[2][1:]] if diff_lines[2].startswith(b"+") else []
    elif len(diff_lines) >= 4 and diff_lines[0] == f"--- {before}".encode() and diff_lines[1] == f"+++ {after}".encode():
        removed = [line[1:] for line in diff_lines if line.startswith(b"-") and not line.startswith(b"---")]
        added = [line[1:] for line in diff_lines if line.startswith(b"+") and not line.startswith(b"+++")]
    else:
        _fail("root_edit.diff", "must name the exact before header and computed line")
    if len(removed) != 1 or len(added) != 1:
        _fail("root_edit.diff", "must contain exactly one changed line")
    before_line_start = before_bytes.rfind(b"\n", 0, prefix) + 1
    before_line_end = before_bytes.find(b"\n", prefix)
    if before_line_end < 0:
        before_line_end = len(before_bytes)
    after_line_start = after_bytes.rfind(b"\n", 0, prefix + 1) + 1
    after_line_end = after_bytes.find(b"\n", prefix + 1)
    if after_line_end < 0:
        after_line_end = len(after_bytes)
    if removed[0] != before_bytes[before_line_start:before_line_end] or added[0] != after_bytes[after_line_start:after_line_end]:
        _fail("root_edit.diff", "changed lines do not match the exact header edit")
    return {
        "after_sha256": _sha256(after),
        "before_sha256": _sha256(before),
        "diff_sha256": _sha256(diff),
        "inserted_byte_hex": f"{inserted:02x}",
        "line_count_after": after_bytes.count(b"\n"),
        "line_count_before": before_bytes.count(b"\n"),
        "path": str(before),
    }


def _trace(path: Path, count: int) -> list[dict[str, Any]]:
    try:
        rows = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        _fail("selection.trace", f"cannot read: {exc}")
    if not rows:
        _fail("selection.trace", "is empty")
    fields = rows[0].split("\t")
    required = {"logical", "ii_relative", "actual_input"}
    if not required <= set(fields):
        _fail("selection.trace", "lacks logical/ii_relative/actual_input columns")
    positions = {field: fields.index(field) for field in required}
    result: list[dict[str, Any]] = []
    seen_logical: set[int] = set()
    seen_relative: set[str] = set()
    seen_actual: set[str] = set()
    for ordinal, line in enumerate(rows[1:]):
        values = line.split("\t")
        if len(values) != len(fields):
            _fail(f"selection.trace[{ordinal}]", "wrong column count")
        try:
            logical = int(values[positions["logical"]])
        except ValueError:
            _fail(f"selection.trace[{ordinal}].logical", "must be an integer")
        relative = _safe_relative(values[positions["ii_relative"]], f"selection.trace[{ordinal}].ii_relative")
        actual = values[positions["actual_input"]]
        _regular(actual, f"selection.trace[{ordinal}].actual_input")
        if logical != ordinal or logical in seen_logical:
            _fail("selection.trace", "logical indexes must be contiguous and unique")
        if relative in seen_relative or actual in seen_actual:
            _fail("selection.trace", "ii and actual-input paths must be unique")
        seen_logical.add(logical)
        seen_relative.add(relative)
        seen_actual.add(actual)
        result.append({"actual_input": actual, "ii_relative": relative, "logical": logical})
    if len(result) < count:
        _fail("selection.trace", f"needs at least {count} rows")
    return result[:count]


def _compile_commands(
    path: Path,
    trace_rows: list[dict[str, Any]],
    a_paths: tuple[Path, ...] | None = None,
    resolve_ambiguous: bool = False,
) -> dict[str, dict[str, Any]]:
    """Validate and index every exact compile-database row used by the trace."""
    return _command_specs(path, trace_rows, a_paths, resolve_ambiguous)


def _same_or_single_insert(before: Path, after: Path, inserted: int) -> tuple[bool, bool]:
    """Return (identical, exact-single-insertion), streaming via mmap."""
    if before.stat().st_size == after.stat().st_size:
        with before.open("rb") as left, after.open("rb") as right:
            while True:
                a, b = left.read(1024 * 1024), right.read(1024 * 1024)
                if a != b:
                    return False, False
                if not a:
                    return True, False
    if after.stat().st_size != before.stat().st_size + 1:
        return False, False
    if before.stat().st_size == 0:
        return False, after.read_bytes() == bytes((inserted,))
    with before.open("rb") as left, after.open("rb") as right:
        with mmap.mmap(left.fileno(), 0, access=mmap.ACCESS_READ) as a, mmap.mmap(
            right.fileno(), 0, access=mmap.ACCESS_READ
        ) as b:
            limit = len(a)
            prefix = 0
            while prefix < limit and a[prefix] == b[prefix]:
                prefix += 1
            if prefix == limit:
                return False, b[prefix] == inserted
            if b[prefix] != inserted:
                return False, False
            chunk = 1024 * 1024
            for offset in range(prefix, limit, chunk):
                end = min(limit, offset + chunk)
                if a[offset:end] != b[offset + 1 : end + 1]:
                    return False, False
            return False, True


COMPILE_ROW_FIELDS = frozenset(
    {
        "actual_input", "argv", "command_sha256", "compiler_path", "compiler_sha256",
        "elapsed_ms", "error", "exit_code", "index", "input_sha256", "object_path",
        "output_sha256", "stderr_first_diagnostic", "stderr_sha256", "stdout_sha256",
        "resolution_basis", "resolution_row_sha256", "timed_out", "turn", "working_directory",
    }
)
COMPILE_DOC_FIELDS = frozenset({"compile_commands_sha256", "count", "executor", "rows", "schema", "source_error", "source_evidence"})
ROOT_EDIT_FIELDS = frozenset(
    {"after_sha256", "before_sha256", "diff_sha256", "inserted_byte_hex", "line_count_after", "line_count_before", "path"}
)
SELECTION_FIELDS = frozenset({"compile_commands_sha256", "count", "rows", "schema", "trace_sha256", "turn_a_root", "turn_b_root"})
SELECTION_ROW_FIELDS = frozenset({"actual_input", "command_sha256", "ii_relative", "index", "resolution_basis", "resolution_row_sha256", "turn_a_path", "turn_b_path"})
PAIR_FIELDS = frozenset({"pairs", "schema"})
PAIR_ROW_FIELDS = frozenset({"affected", "index", "path", "turn_a_sha256", "turn_b_sha256"})
FILE_BIND_FIELDS = frozenset({"path", "sha256"})
COMPILE_BIND_FIELDS = frozenset({"path", "sha256", "turn_a_pass", "turn_b_pass"})


def _first_diagnostic(stderr: bytes) -> str:
    lines = [
        line.strip()
        for line in stderr.decode("utf-8", "replace").splitlines()
        if line.strip()
    ]
    error = next(
        (
            line
            for line in lines
            if "fatal error:" in line.lower() or "error:" in line.lower()
        ),
        None,
    )
    return (error or next(iter(lines), ""))[:1000]


def _difference_summary(retained: bytes, reproduced: bytes) -> str:
    """Return bounded, deterministic evidence for unequal preprocessor output."""
    common = min(len(retained), len(reproduced))
    offset = next(
        (index for index in range(common) if retained[index] != reproduced[index]),
        common,
    )
    start = max(0, offset - 16)
    end = offset + 17
    return (
        f"first_difference={offset};retained_bytes={len(retained)};"
        f"reproduced_bytes={len(reproduced)};"
        f"retained_hex={retained[start:end].hex()};"
        f"reproduced_hex={reproduced[start:end].hex()}"
    )


def _compile_one(
    *,
    index: int,
    turn: str,
    input_path: Path,
    actual_input: str,
    object_path: Path,
    spec: Mapping[str, Any],
    timeout_s: float,
) -> dict[str, Any]:
    started = time.monotonic_ns()
    observed_input_sha = _sha256(input_path)
    argv = _reconstruct_argv(spec, input_path, object_path)
    stdout = b""
    stderr = b""
    error = ""
    timed_out = False
    exit_code: int | None = None
    try:
        process = subprocess.run(
            argv,
            cwd=spec["directory"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
        )
        exit_code = process.returncode
        stdout, stderr = process.stdout, process.stderr
    except subprocess.TimeoutExpired as exc:
        exit_code = 124
        timed_out = True
        error = "timeout"
        stdout = exc.stdout or b""
        stderr = exc.stderr or b""
    except OSError as exc:
        exit_code = 125
        error = f"{type(exc).__name__}: {exc}"
    elapsed_ms = round((time.monotonic_ns() - started) / 1_000_000, 3)
    output_sha = None
    if exit_code == 0 and not timed_out:
        if not object_path.is_file() or object_path.is_symlink():
            error = "compiler reported success without a regular object"
            exit_code = 125
        else:
            output_sha = _sha256(object_path)
    if object_path.exists() or object_path.is_symlink():
        object_path.unlink(missing_ok=True)
    return {
        "actual_input": actual_input,
        "argv": argv,
        "command_sha256": spec["command_sha256"],
        "compiler_path": spec["compiler_path"],
        "compiler_sha256": spec["compiler_sha256"],
        "elapsed_ms": elapsed_ms,
        "error": error,
        "exit_code": exit_code,
        "index": index,
        "input_sha256": observed_input_sha,
        "object_path": str(object_path),
        "output_sha256": output_sha,
        "resolution_basis": spec["resolution_basis"],
        "resolution_row_sha256": spec["resolution_row_sha256"],
        "stderr_first_diagnostic": _first_diagnostic(stderr),
        "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
        "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
        "timed_out": timed_out,
        "turn": turn,
        "working_directory": spec["directory"],
    }


def _write_once_json(path: Path, value: Mapping[str, Any]) -> None:
    if path.exists() or path.is_symlink():
        raise RootHeaderAuthorityError(f"receipt already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as handle:
            handle.write(canonical_bytes(value))
    except FileExistsError as exc:
        raise RootHeaderAuthorityError(f"receipt already exists: {path}") from exc


def _publish_json_noreplace(path: Path, value: Mapping[str, Any]) -> None:
    """Publish a receipt without replacing a destination that raced us."""
    if path.exists() or path.is_symlink():
        raise RootHeaderAuthorityError(f"receipt already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.parent / f".{path.name}.publish-{os.getpid()}-{secrets.token_hex(8)}"
    try:
        with temporary.open("xb") as handle:
            handle.write(canonical_bytes(value))
            handle.flush()
            os.fsync(handle.fileno())
        os.link(temporary, path)
    except FileExistsError as exc:
        raise RootHeaderAuthorityError(f"receipt appeared during publication: {path}") from exc
    finally:
        temporary.unlink(missing_ok=True)


def _publish_directory_noreplace(source: Path, destination: Path) -> None:
    """Exclusively publish a directory without replacing a raced destination."""
    if os.name != "posix" or sys.platform != "linux":
        raise RootHeaderAuthorityError(
            "exclusive directory publication requires Linux renameat2"
        )
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise RootHeaderAuthorityError("libc has no renameat2 entry point")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(
        -100, os.fsencode(source), -100, os.fsencode(destination), 1
    )
    if result == 0:
        return
    error = ctypes.get_errno()
    if error == errno.EEXIST:
        raise RootHeaderAuthorityError(
            f"output appeared during exclusive publication: {destination}"
        )
    if error not in (errno.EINVAL, errno.ENOSYS, errno.EOPNOTSUPP):
        raise RootHeaderAuthorityError(
            f"exclusive directory publication failed: {os.strerror(error)}"
        )
    # Some scratch filesystems expose renameat2 but reject RENAME_NOREPLACE.
    # Reserve the destination exclusively, then hard-link every regular file;
    # link(2) is itself no-overwrite.  This fallback is deliberately
    # fail-closed on any collision and never uses os.replace.
    try:
        destination.mkdir(mode=0o700, exist_ok=False)
        for item in sorted(source.rglob("*"), key=lambda path: (not path.is_dir(), str(path))):
            relative = item.relative_to(source)
            target = destination / relative
            if item.is_dir() and not item.is_symlink():
                target.mkdir(mode=0o700, exist_ok=False)
            elif item.is_file() and not item.is_symlink():
                os.link(item, target)
            else:
                raise RootHeaderAuthorityError(
                    f"unsafe publication member: {item}"
                )
        shutil.rmtree(source)
        directory_fd = os.open(destination.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except FileExistsError as exc:
        raise RootHeaderAuthorityError(
            f"output appeared during exclusive publication: {destination}"
        ) from exc
    except BaseException:
        shutil.rmtree(destination, ignore_errors=True)
        raise


def run_compile_validation(
    *,
    source_root: Path,
    source_commit: str,
    root_include: Path,
    trace_path: Path,
    compile_commands_path: Path,
    turn_a_manifest: Path,
    turn_b_manifest: Path,
    output_path: Path,
    count: int = 1000,
    timeout_s: float = 120.0,
    jobs: int = 4,
) -> dict[str, Any]:
    """Compile retained A/B inputs with exact argv reconstruction, never a shell."""
    if count < 1 or jobs < 1 or timeout_s <= 0:
        _fail("compile_validation", "count, jobs, and timeout must be positive")
    git_start = _git_worktree(source_root, source_commit)
    header_start = _source_header_evidence(source_root, source_commit, root_include)
    trace_rows = _trace(trace_path, count)
    a_paths = _manifest(turn_a_manifest, count, "turn_a_manifest")
    b_paths = _manifest(turn_b_manifest, count, "turn_b_manifest")
    specs = _compile_commands(compile_commands_path, trace_rows, a_paths, True)
    output_path = output_path.absolute()
    if output_path.exists() or output_path.is_symlink():
        _fail("compile_validation.output", "receipt already exists")
    stage = output_path.parent / f".{output_path.name}.objects"
    if stage.exists() or stage.is_symlink():
        _fail("compile_validation.output", "object staging path already exists")
    stage.mkdir(parents=True, exist_ok=False)
    rows: list[dict[str, Any]] = []
    futures = {}
    try:
        with ThreadPoolExecutor(max_workers=min(jobs, 2 * count)) as pool:
            for index, (actual, a_path, b_path) in enumerate(
                zip((row["actual_input"] for row in trace_rows), a_paths, b_paths, strict=True)
            ):
                spec = specs[actual]
                for turn, input_path in (("A", a_path), ("B", b_path)):
                    object_path = stage / turn / f"{index:04d}.o"
                    object_path.parent.mkdir(parents=True, exist_ok=True)
                    futures[pool.submit(
                        _compile_one,
                        index=index,
                        turn=turn,
                        input_path=input_path,
                        actual_input=actual,
                        object_path=object_path,
                        spec=spec,
                        timeout_s=timeout_s,
                    )] = (index, turn)
            for future in as_completed(futures):
                rows.append(future.result())
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    source_error = ""
    try:
        git_end = _git_snapshot(source_root)
        header_end = _source_header_snapshot(source_root, source_commit, root_include)
        if git_end["head"] != source_commit or git_end["status"]:
            source_error = "source Git worktree was not clean at compile end"
        elif not header_end["tracked"] or header_end["committed_sha256"] != header_end["current_sha256"]:
            source_error = "source root header changed at compile end"
        compiler_end_cache: dict[str, tuple[str, str]] = {}
        for spec in specs.values():
            if spec["compiler_path"] not in compiler_end_cache:
                compiler_end_cache[spec["compiler_path"]] = _compiler(spec["compiler_path"], "compile.compiler.end")
            compiler_path, compiler_sha256 = compiler_end_cache[spec["compiler_path"]]
            if compiler_path != spec["compiler_path"] or compiler_sha256 != spec["compiler_sha256"]:
                source_error = "compiler identity changed at compile end"
    except RootHeaderAuthorityError as exc:
        source_error = str(exc)
        git_end = {"head": "", "status": "probe-error", "status_sha256": hashlib.sha256(b"probe-error").hexdigest()}
        header_end = {
            "committed_sha256": None,
            "current_sha256": "",
            "path": str(root_include),
            "relative": "",
            "tracked": False,
        }
    rows.sort(key=lambda row: (row["index"], row["turn"]))
    receipt = {
        "compile_commands_sha256": _sha256(compile_commands_path),
        "count": count,
        "executor": {"jobs": jobs, "timeout_s": timeout_s},
        "rows": rows,
        "schema": COMPILE_SCHEMA,
        "source_error": source_error,
        "source_evidence": {
            "commit": source_commit,
            "git_end": git_end,
            "git_start": git_start,
            "header_end": header_end,
            "header_start": header_start,
            "root": str(source_root),
        },
    }
    _write_once_json(output_path, receipt)
    return receipt


_RETAINED_NAMESPACE_SCRIPT = r'''
import hashlib
import json
import subprocess
import sys

payload = json.loads(sys.argv[1])
source = payload["edited_header"]
target = payload["root_include"]
argv = payload["compile_argv"]
cwd = payload["working_directory"]
mountinfo_path = payload["mountinfo_path"]
mount_executable = payload["mount_executable"]
umount_executable = payload["umount_executable"]
mounted = False
try:
    mount = subprocess.run([mount_executable, "--bind", source, target], check=False,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if mount.returncode != 0:
        sys.stderr.buffer.write(b"ICEFARM-MOUNT-ERROR\n" + mount.stderr)
        raise SystemExit(125)
    mounted = True
    mountinfo = open("/proc/self/mountinfo", "rb").read()
    mount_rows = []
    for raw_row in mountinfo.splitlines():
        fields = raw_row.decode("utf-8", "replace").split(" - ", 1)[0].split()
        if len(fields) >= 5:
            mountpoint = fields[4].replace(r"\040", " ").replace(r"\011", "\t").replace(r"\134", "\\")
            if mountpoint == target:
                mount_rows.append(raw_row)
    if len(mount_rows) != 1:
        raise RuntimeError("expected exactly one root-header mountinfo row")
    source_stat = __import__("os").stat(source, follow_symlinks=False)
    target_stat = __import__("os").stat(target, follow_symlinks=False)
    bind_evidence = {
        "bind_device": target_stat.st_dev,
        "bind_inode": target_stat.st_ino,
        "bind_source_sha256": hashlib.sha256(open(source, "rb").read()).hexdigest(),
        "bind_target_sha256": hashlib.sha256(open(target, "rb").read()).hexdigest(),
        "mountinfo_row_sha256": hashlib.sha256(mount_rows[0]).hexdigest(),
        "source_device": source_stat.st_dev,
        "source_inode": source_stat.st_ino,
        "target_device": target_stat.st_dev,
        "target_inode": target_stat.st_ino,
    }
    if bind_evidence["bind_source_sha256"] != bind_evidence["bind_target_sha256"]:
        raise RuntimeError("root-header bind content identity mismatch")
    if (source_stat.st_dev, source_stat.st_ino) != (target_stat.st_dev, target_stat.st_ino):
        raise RuntimeError("root-header bind inode identity mismatch")
    with open(mountinfo_path, "xb") as handle:
        handle.write(mountinfo)
    sys.stderr.buffer.write(
        b"ICEFARM-MOUNTINFO-SHA256:" + hashlib.sha256(mountinfo).hexdigest().encode() + b"\n"
    )
    sys.stderr.buffer.write(
        b"ICEFARM-BIND-EVIDENCE:" + json.dumps(bind_evidence, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    )
    process = subprocess.run(argv, cwd=cwd, check=False,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sys.stdout.buffer.write(process.stdout)
    sys.stderr.buffer.write(process.stderr)
    exit_code = process.returncode
finally:
    if mounted:
        unmount = subprocess.run([umount_executable, target], check=False,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if unmount.returncode != 0 and "exit_code" in locals() and exit_code == 0:
            exit_code = 125
if "exit_code" in locals():
    raise SystemExit(exit_code)
raise SystemExit(125)
'''


def _namespace_argv(payload: Mapping[str, Any]) -> list[str]:
    return [*NAMESPACE_TIMEOUT_ARGV, os.fspath(Path(sys.executable).resolve()), "-c", _RETAINED_NAMESPACE_SCRIPT,
            canonical_bytes(payload).decode("utf-8")]


def _system_tool(name: str, subject: str) -> tuple[str, str]:
    resolved = shutil.which(name)
    if not resolved:
        _fail(subject, f"{name} executable is unavailable")
    return _compiler(resolved, subject)


def _default_namespace_invoke(argv: list[str], timeout_s: float) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        argv,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout_s,
    )


def _mountinfo_hash(stderr: bytes) -> tuple[str, bytes]:
    marker = re.compile(rb"^ICEFARM-MOUNTINFO-SHA256:([0-9a-f]{64})\n")
    match = marker.match(stderr)
    if match is None:
        return "", stderr
    return match.group(1).decode("ascii"), stderr


def _bind_evidence(stderr: bytes) -> dict[str, Any] | None:
    marker = b"ICEFARM-BIND-EVIDENCE:"
    lines = [line[len(marker):] for line in stderr.splitlines() if line.startswith(marker)]
    if len(lines) != 1:
        return None
    try:
        value = json.loads(lines[0])
    except (TypeError, ValueError):
        return None
    if not isinstance(value, dict):
        return None
    return value


def _mountinfo_target_row(mountinfo: bytes, target: str) -> bytes | None:
    rows: list[bytes] = []
    for raw_row in mountinfo.splitlines():
        fields = raw_row.decode("utf-8", "replace").split(" - ", 1)[0].split()
        if len(fields) < 5:
            continue
        mountpoint = fields[4].replace(r"\040", " ").replace(r"\011", "\t").replace(r"\134", "\\")
        if mountpoint == target:
            rows.append(raw_row)
    return rows[0] if len(rows) == 1 else None


def _retained_one(
    *,
    index: int,
    actual_input: str,
    spec: Mapping[str, Any],
    b_path: Path,
    header_after: Path,
    root_include: Path,
    mountinfo_path: Path,
    stderr_path: Path,
    mount_executable: str,
    mount_executable_sha256: str,
    umount_executable: str,
    umount_executable_sha256: str,
    timeout_s: float,
    invoke: Callable[[list[str], float], subprocess.CompletedProcess[bytes]],
) -> dict[str, Any]:
    started = time.monotonic_ns()
    input_path = Path(actual_input)
    compile_argv = _preprocess_argv(spec, input_path)
    payload = {
        "compile_argv": compile_argv,
        "edited_header": str(header_after),
        "root_include": str(root_include),
        "mount_executable": mount_executable,
        "mountinfo_path": str(mountinfo_path),
        "umount_executable": umount_executable,
        "working_directory": spec["directory"],
    }
    namespace_argv = _namespace_argv(payload)
    mount_argv = [mount_executable, "--bind", str(header_after), str(root_include)]
    unmount_argv = [umount_executable, str(root_include)]
    stdout = b""
    stderr = b""
    error = ""
    exit_code = 125
    timed_out = False
    try:
        result = invoke(namespace_argv, timeout_s)
        exit_code = result.returncode
        stdout = result.stdout if isinstance(result.stdout, bytes) else str(result.stdout or "").encode()
        stderr = result.stderr if isinstance(result.stderr, bytes) else str(result.stderr or "").encode()
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        exit_code = 124
        error = "timeout"
        stdout = exc.stdout or b""
        stderr = exc.stderr or b""
    except OSError as exc:
        error = f"{type(exc).__name__}: {exc}"
    if not isinstance(stdout, bytes):
        stdout = str(stdout or "").encode()
    if not isinstance(stderr, bytes):
        stderr = str(stderr or "").encode()
    elapsed_ms = round((time.monotonic_ns() - started) / 1_000_000, 3)
    stderr_path.write_bytes(stderr)
    mountinfo_sha256, _ = _mountinfo_hash(stderr)
    bind_evidence = _bind_evidence(stderr)
    if not mountinfo_sha256 and not error:
        error = "missing mountinfo evidence"
    elif not mountinfo_path.is_file() or mountinfo_path.is_symlink() or _sha256(mountinfo_path) != mountinfo_sha256:
        error = "mountinfo evidence hash mismatch"
    reproduced_sha = hashlib.sha256(stdout).hexdigest() if exit_code == 0 and not timed_out else None
    retained_sha = _sha256(b_path)
    equal = reproduced_sha == retained_sha if reproduced_sha is not None else False
    if exit_code != 0 or timed_out:
        if not error:
            error = _first_diagnostic(stderr) or f"namespace exited {exit_code}"
    elif not equal:
        error = (
            "reproduced output does not equal retained B: "
            + _difference_summary(b_path.read_bytes(), stdout)
        )
    if bind_evidence is None and not error:
        error = "missing bind identity evidence"
    bind_evidence = bind_evidence or {
        "bind_device": -1, "bind_inode": -1, "bind_source_sha256": "", "bind_target_sha256": "",
        "mountinfo_row_sha256": "", "source_device": -1, "source_inode": -1,
        "target_device": -1, "target_inode": -1,
    }
    return {
        "actual_input": actual_input,
        "argv": compile_argv,
        "bind_device": bind_evidence["bind_device"],
        "bind_inode": bind_evidence["bind_inode"],
        "bind_source_sha256": bind_evidence["bind_source_sha256"],
        "bind_target_sha256": bind_evidence["bind_target_sha256"],
        "command_sha256": spec["command_sha256"],
        "compiler_path": spec["compiler_path"],
        "compiler_sha256": spec["compiler_sha256"],
        "elapsed_ms": elapsed_ms,
        "error": error,
        "equal": equal,
        "exit_code": exit_code,
        "index": index,
        "input_sha256": _sha256(input_path),
        "mount_argv": mount_argv,
        "mount_executable": mount_executable,
        "mount_executable_sha256": mount_executable_sha256,
        "mountinfo_path": str(mountinfo_path),
        "mountinfo_row_sha256": bind_evidence["mountinfo_row_sha256"],
        "mountinfo_sha256": mountinfo_sha256,
        "namespace_argv": namespace_argv,
        "reproduced_sha256": reproduced_sha,
        "resolution_basis": spec["resolution_basis"],
        "resolution_row_sha256": spec["resolution_row_sha256"],
        "retained_path": str(b_path),
        "retained_sha256": retained_sha,
        "stderr_first_diagnostic": _first_diagnostic(stderr),
        "stderr_path": str(stderr_path),
        "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
        "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
        "timed_out": timed_out,
        "turn": "B",
        "unmount_argv": unmount_argv,
        "umount_executable": umount_executable,
        "umount_executable_sha256": umount_executable_sha256,
        "working_directory": spec["directory"],
        "source_device": bind_evidence["source_device"],
        "source_inode": bind_evidence["source_inode"],
        "target_device": bind_evidence["target_device"],
        "target_inode": bind_evidence["target_inode"],
    }


def run_retained_true_path_revalidation(
    *,
    source_root: Path,
    source_commit: str,
    root_include: Path,
    header_after: Path,
    trace_path: Path,
    compile_commands_path: Path,
    turn_a_manifest: Path,
    turn_b_manifest: Path,
    output_path: Path,
    count: int = 1000,
    timeout_s: float = 120.0,
    jobs: int = 4,
    invoke: Callable[[list[str], float], subprocess.CompletedProcess[bytes]] | None = None,
) -> dict[str, Any]:
    """Reproduce retained B through a real private mount namespace.

    ``invoke`` exists solely for bounded tests.  Production uses the argv-only
    subprocess path and never regenerates or overwrites retained corpus files.
    """
    if count < 1 or jobs < 1 or timeout_s <= 0:
        _fail("retained_execution", "count, jobs, and timeout must be positive")
    source_root = _directory(str(source_root), "source.root")
    git_start = _git_worktree(source_root, source_commit)
    header_start = _source_header_evidence(source_root, source_commit, root_include)
    header_after = _regular(str(header_after), "provenance.edited_header")
    header_after_start_sha = _sha256(header_after)
    trace_rows = _trace(trace_path, count)
    a_paths = _manifest(turn_a_manifest, count, "turn_a_manifest")
    b_paths = _manifest(turn_b_manifest, count, "turn_b_manifest")
    specs = _compile_commands(compile_commands_path, trace_rows, a_paths, True)
    output_path = output_path.absolute()
    if output_path.exists() or output_path.is_symlink():
        _fail("retained_execution.output", "receipt already exists")
    mount_executable, mount_executable_sha256 = _system_tool("mount", "retained.mount")
    umount_executable, umount_executable_sha256 = _system_tool("umount", "retained.umount")
    mountinfo_dir = output_path.with_suffix(".mountinfo")
    stderr_dir = output_path.with_suffix(".stderr")
    if mountinfo_dir.exists() or mountinfo_dir.is_symlink():
        _fail("retained_execution.output", "mountinfo evidence already exists")
    if stderr_dir.exists() or stderr_dir.is_symlink():
        _fail("retained_execution.output", "stderr evidence already exists")
    mountinfo_dir.mkdir(parents=True, exist_ok=False)
    stderr_dir.mkdir(parents=True, exist_ok=False)
    invoke = invoke or _default_namespace_invoke
    rows: list[dict[str, Any]] = []
    futures = {}
    with ThreadPoolExecutor(max_workers=min(jobs, count)) as pool:
        for index, (trace, b_path) in enumerate(zip(trace_rows, b_paths, strict=True)):
            futures[pool.submit(
                _retained_one,
                index=index,
                actual_input=trace["actual_input"],
                spec=specs[trace["actual_input"]],
                b_path=b_path,
                header_after=header_after,
                root_include=root_include,
                mountinfo_path=mountinfo_dir / f"{index:04d}.mountinfo",
                stderr_path=stderr_dir / f"{index:04d}.stderr",
                mount_executable=mount_executable,
                mount_executable_sha256=mount_executable_sha256,
                umount_executable=umount_executable,
                umount_executable_sha256=umount_executable_sha256,
                timeout_s=timeout_s,
                invoke=invoke,
            )] = index
        for future in as_completed(futures):
            rows.append(future.result())
    source_error = ""
    try:
        git_end = _git_snapshot(source_root)
        header_end = _source_header_snapshot(source_root, source_commit, root_include)
        header_after_end_sha = _sha256(header_after)
        if git_end["head"] != source_commit or git_end["status"]:
            source_error = "source Git worktree was not clean at retained revalidation end"
        elif not header_end["tracked"] or header_end["committed_sha256"] != header_end["current_sha256"]:
            source_error = "source root header changed at retained revalidation end"
        elif header_after_end_sha != header_after_start_sha:
            source_error = "edited header changed at retained revalidation end"
        compiler_end_cache: dict[str, tuple[str, str]] = {}
        for spec in specs.values():
            if spec["compiler_path"] not in compiler_end_cache:
                compiler_end_cache[spec["compiler_path"]] = _compiler(spec["compiler_path"], "retained.compiler.end")
            if compiler_end_cache[spec["compiler_path"]] != (spec["compiler_path"], spec["compiler_sha256"]):
                source_error = "compiler identity changed at retained revalidation end"
    except RootHeaderAuthorityError as exc:
        source_error = str(exc)
        git_end = {"head": "", "status": "probe-error", "status_sha256": hashlib.sha256(b"probe-error").hexdigest()}
        header_end = {"committed_sha256": None, "current_sha256": "", "path": str(root_include), "relative": "", "tracked": False}
        header_after_end_sha = ""
    rows.sort(key=lambda row: row["index"])
    receipt = {
        "compile_commands_sha256": _sha256(compile_commands_path),
        "count": count,
        "executor": {"jobs": jobs, "timeout_s": timeout_s},
        "rows": rows,
        "schema": RETAINED_EXECUTION_SCHEMA,
        "source_error": source_error,
        "source_evidence": {
            "commit": source_commit,
            "edited_header_end_sha256": header_after_end_sha,
            "edited_header_start_sha256": header_after_start_sha,
            "git_end": git_end,
            "git_start": git_start,
            "header_end": header_end,
            "header_start": header_start,
            "root": str(source_root),
        },
    }
    _write_once_json(output_path, receipt)
    return receipt


def _retained_receipt(
    path: Path,
    count: int,
    source_root: Path,
    source_commit: str,
    root_include: Path,
    header_after: Path,
    a_paths: tuple[Path, ...],
    b_paths: tuple[Path, ...],
    trace_rows: list[dict[str, Any]],
    command_specs: dict[str, dict[str, Any]],
    compile_commands_sha256: str,
) -> dict[str, Any]:
    value = _json(path, "retained_execution")
    if not isinstance(value, dict) or value.get("schema") != RETAINED_EXECUTION_SCHEMA:
        _fail("retained_execution", f"expected schema {RETAINED_EXECUTION_SCHEMA!r}")
    if set(value) != RETAINED_DOC_FIELDS:
        _fail("retained_execution", "fields are not exact")
    if value.get("compile_commands_sha256") != compile_commands_sha256 or value.get("count") != count:
        _fail("retained_execution", "compile database or count binding mismatch")
    if value.get("source_error") != "":
        _fail("retained_execution.source_error", "source/compiler identity was not clean")
    source_evidence = _mapping(value.get("source_evidence"), "retained_execution.source_evidence")
    expected_source_fields = {
        "commit", "edited_header_end_sha256", "edited_header_start_sha256", "git_end", "git_start",
        "header_end", "header_start", "root",
    }
    if set(source_evidence) != expected_source_fields:
        _fail("retained_execution.source_evidence", "fields are not exact")
    if source_evidence["commit"] != source_commit or source_evidence["root"] != str(source_root):
        _fail("retained_execution.source_evidence", "source binding mismatch")
    _sha(source_evidence["edited_header_start_sha256"], "retained_execution.source_evidence.edited_header_start_sha256")
    _sha(source_evidence["edited_header_end_sha256"], "retained_execution.source_evidence.edited_header_end_sha256")
    observed_git = _git_worktree(source_root, source_commit)
    observed_header = _source_header_evidence(source_root, source_commit, root_include)
    for field in ("git_start", "git_end"):
        if source_evidence[field] != observed_git:
            _fail(f"retained_execution.source_evidence.{field}", "Git evidence mismatch")
    for field in ("header_start", "header_end"):
        if source_evidence[field] != observed_header:
            _fail(f"retained_execution.source_evidence.{field}", "header evidence mismatch")
    current_after_sha = _sha256(header_after)
    if source_evidence["edited_header_start_sha256"] != current_after_sha or source_evidence["edited_header_end_sha256"] != current_after_sha:
        _fail("retained_execution.source_evidence", "edited header changed")
    executor = _mapping(value.get("executor"), "retained_execution.executor")
    if set(executor) != RETAINED_EXECUTOR_FIELDS:
        _fail("retained_execution.executor", "fields are not exact")
    if (
        not isinstance(executor["jobs"], int) or isinstance(executor["jobs"], bool) or executor["jobs"] < 1
        or not isinstance(executor["timeout_s"], (int, float)) or isinstance(executor["timeout_s"], bool)
        or executor["timeout_s"] <= 0
    ):
        _fail("retained_execution.executor", "invalid executor metadata")
    rows = value.get("rows")
    if not isinstance(rows, list) or len(rows) != count:
        _fail("retained_execution.rows", f"expected {count} rows")
    seen: set[int] = set()
    compiler_verified: set[str] = set()
    for raw in rows:
        row = _mapping(raw, "retained_execution.rows")
        if set(row) != RETAINED_ROW_FIELDS:
            _fail("retained_execution.rows", "row fields are not exact")
        index = row.get("index")
        if not isinstance(index, int) or isinstance(index, bool) or not 0 <= index < count or index in seen:
            _fail("retained_execution.rows", "invalid or duplicate index")
        seen.add(index)
        if row.get("turn") != "B":
            _fail(f"retained_execution.rows[{index}]", "only B rows are accepted")
        actual = trace_rows[index]["actual_input"]
        spec = command_specs.get(actual)
        if spec is None:
            _fail(f"retained_execution.rows[{index}]", "missing command binding")
        expected_b = b_paths[index]
        if row.get("actual_input") != actual or row.get("retained_path") != str(expected_b):
            _fail(f"retained_execution.rows[{index}]", "input/path binding mismatch")
        if row.get("input_sha256") != _sha256(Path(actual)) or row.get("retained_sha256") != _sha256(expected_b):
            _fail(f"retained_execution.rows[{index}]", "input hash binding mismatch")
        if row.get("command_sha256") != spec["command_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "compile command binding mismatch")
        if row.get("resolution_basis") != spec["resolution_basis"] or row.get("resolution_row_sha256") != spec["resolution_row_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "compile resolution binding mismatch")
        expected_argv = _preprocess_argv(spec, Path(actual))
        if row.get("argv") != expected_argv or row.get("working_directory") != spec["directory"]:
            _fail(f"retained_execution.rows[{index}]", "reconstructed argv or directory mismatch")
        payload = {
            "compile_argv": expected_argv,
            "edited_header": str(header_after),
            "mount_executable": row.get("mount_executable"),
            "mountinfo_path": str(path.with_suffix(".mountinfo") / f"{index:04d}.mountinfo"),
            "root_include": str(root_include),
            "umount_executable": row.get("umount_executable"),
            "working_directory": spec["directory"],
        }
        if row.get("namespace_argv") != _namespace_argv(payload):
            _fail(f"retained_execution.rows[{index}]", "namespace argv binding mismatch")
        mount_executable = row.get("mount_executable")
        umount_executable = row.get("umount_executable")
        mount_sha = row.get("mount_executable_sha256")
        umount_sha = row.get("umount_executable_sha256")
        if not isinstance(mount_executable, str) or not Path(mount_executable).is_absolute() or mount_executable != payload["mount_executable"]:
            _fail(f"retained_execution.rows[{index}]", "mount executable is not absolute")
        if not isinstance(umount_executable, str) or not Path(umount_executable).is_absolute() or umount_executable != payload["umount_executable"]:
            _fail(f"retained_execution.rows[{index}]", "umount executable is not absolute")
        if _compiler(mount_executable, f"retained_execution.rows[{index}].mount_executable") != (mount_executable, mount_sha):
            _fail(f"retained_execution.rows[{index}]", "mount executable identity mismatch")
        if _compiler(umount_executable, f"retained_execution.rows[{index}].umount_executable") != (umount_executable, umount_sha):
            _fail(f"retained_execution.rows[{index}]", "umount executable identity mismatch")
        if row.get("mount_argv") != [mount_executable, "--bind", str(header_after), str(root_include)] or row.get("unmount_argv") != [umount_executable, str(root_include)]:
            _fail(f"retained_execution.rows[{index}]", "mount argv binding mismatch")
        compiler_path = row.get("compiler_path")
        if compiler_path != spec["compiler_path"] or row.get("compiler_sha256") != spec["compiler_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "compiler binding mismatch")
        if compiler_path not in compiler_verified:
            _bound_file(compiler_path, row.get("compiler_sha256"), f"retained_execution.rows[{index}].compiler")
            compiler_verified.add(compiler_path)
        if (
            not isinstance(row.get("elapsed_ms"), (int, float)) or isinstance(row["elapsed_ms"], bool) or row["elapsed_ms"] < 0
            or not isinstance(row.get("exit_code"), int) or isinstance(row["exit_code"], bool)
            or row.get("timed_out") is not False or row.get("error") != ""
            or row.get("equal") is not True or row.get("exit_code") != 0
        ):
            _fail(f"retained_execution.rows[{index}]", "row is not a definitive pass")
        _sha(row.get("reproduced_sha256"), f"retained_execution.rows[{index}].reproduced_sha256")
        if row["reproduced_sha256"] != row["retained_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "reproduced hash mismatch")
        _sha(row.get("retained_sha256"), f"retained_execution.rows[{index}].retained_sha256")
        _sha(row.get("stdout_sha256"), f"retained_execution.rows[{index}].stdout_sha256")
        _sha(row.get("stderr_sha256"), f"retained_execution.rows[{index}].stderr_sha256")
        _sha(row.get("mountinfo_sha256"), f"retained_execution.rows[{index}].mountinfo_sha256")
        _sha(row.get("mountinfo_row_sha256"), f"retained_execution.rows[{index}].mountinfo_row_sha256")
        expected_mountinfo = path.with_suffix(".mountinfo") / f"{index:04d}.mountinfo"
        if row.get("mountinfo_path") != str(expected_mountinfo):
            _fail(f"retained_execution.rows[{index}]", "mountinfo path binding mismatch")
        mountinfo = _regular(row.get("mountinfo_path"), f"retained_execution.rows[{index}].mountinfo_path")
        mountinfo_bytes = mountinfo.read_bytes()
        if _sha256(mountinfo) != row["mountinfo_sha256"] or mountinfo.stat().st_size == 0:
            _fail(f"retained_execution.rows[{index}]", "mountinfo evidence hash mismatch")
        target_row = _mountinfo_target_row(mountinfo_bytes, str(root_include))
        if target_row is None or hashlib.sha256(target_row).hexdigest() != row["mountinfo_row_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "mountinfo target-row binding mismatch")
        expected_stderr = path.with_suffix(".stderr") / f"{index:04d}.stderr"
        if row.get("stderr_path") != str(expected_stderr):
            _fail(f"retained_execution.rows[{index}]", "stderr path binding mismatch")
        stderr = _regular(row.get("stderr_path"), f"retained_execution.rows[{index}].stderr_path")
        if _sha256(stderr) != row["stderr_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "stderr evidence hash mismatch")
        stderr_bytes = stderr.read_bytes()
        observed_mountinfo_sha, _ = _mountinfo_hash(stderr_bytes)
        if observed_mountinfo_sha != row["mountinfo_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "stderr mountinfo binding mismatch")
        observed_bind = _bind_evidence(stderr_bytes)
        bind_fields = (
            "bind_device", "bind_inode", "bind_source_sha256", "bind_target_sha256", "mountinfo_row_sha256",
            "source_device", "source_inode", "target_device", "target_inode",
        )
        if observed_bind is None or any(observed_bind.get(field) != row[field] for field in bind_fields):
            _fail(f"retained_execution.rows[{index}]", "stderr bind identity mismatch")
        if _first_diagnostic(stderr_bytes) != row["stderr_first_diagnostic"]:
            _fail(f"retained_execution.rows[{index}]", "stderr diagnostic binding mismatch")
        bind_sha = _sha256(header_after)
        if row["bind_source_sha256"] != bind_sha or row["bind_target_sha256"] != bind_sha:
            _fail(f"retained_execution.rows[{index}]", "bind content identity mismatch")
        for field in ("bind_device", "bind_inode", "source_device", "source_inode", "target_device", "target_inode"):
            if not isinstance(row[field], int) or isinstance(row[field], bool) or row[field] < 0:
                _fail(f"retained_execution.rows[{index}]", "bind stat identity is invalid")
        if row["source_device"] != row["target_device"] or row["source_inode"] != row["target_inode"]:
            _fail(f"retained_execution.rows[{index}]", "bind stat identity mismatch")
        if (row["bind_device"], row["bind_inode"]) != (row["target_device"], row["target_inode"]):
            _fail(f"retained_execution.rows[{index}]", "bind target stat mismatch")
        if row["stdout_sha256"] != row["reproduced_sha256"]:
            _fail(f"retained_execution.rows[{index}]", "stdout does not bind reproduced output")
        if not isinstance(row.get("stderr_first_diagnostic"), str):
            _fail(f"retained_execution.rows[{index}]", "diagnostic is not a string")
    if len(seen) != count:
        _fail("retained_execution.rows", "missing index")
    return value


def _compile_receipt(
    path: Path,
    count: int,
    source_root: Path,
    source_commit: str,
    root_include: Path,
    a_paths: tuple[Path, ...],
    b_paths: tuple[Path, ...],
    trace_rows: list[dict[str, Any]],
    command_specs: dict[str, dict[str, Any]],
    compile_commands_sha256: str,
) -> dict[str, Any]:
    value = _json(path, "compile_validation")
    if not isinstance(value, dict) or value.get("schema") != COMPILE_SCHEMA:
        _fail("compile_validation", f"expected schema {COMPILE_SCHEMA!r}")
    if set(value) != COMPILE_DOC_FIELDS:
        _fail("compile_validation", "fields are not exact")
    if value.get("compile_commands_sha256") != compile_commands_sha256:
        _fail("compile_validation", "compile database hash mismatch")
    if value.get("count") != count:
        _fail("compile_validation", "count mismatch")
    if value.get("source_error") != "":
        _fail("compile_validation.source_error", "source/compiler identity was not clean")
    source_evidence = _mapping(value.get("source_evidence"), "compile_validation.source_evidence")
    if set(source_evidence) != {"commit", "git_end", "git_start", "header_end", "header_start", "root"}:
        _fail("compile_validation.source_evidence", "fields are not exact")
    if source_evidence["commit"] != source_commit or source_evidence["root"] != str(source_root):
        _fail("compile_validation.source_evidence", "source binding mismatch")
    observed_git = _git_worktree(source_root, source_commit)
    observed_header = _source_header_evidence(source_root, source_commit, root_include)
    for field in ("git_start", "git_end"):
        if source_evidence[field] != observed_git:
            _fail(f"compile_validation.source_evidence.{field}", "Git evidence mismatch")
    for field in ("header_start", "header_end"):
        if source_evidence[field] != observed_header:
            _fail(f"compile_validation.source_evidence.{field}", "header evidence mismatch")
    executor = _mapping(value.get("executor"), "compile_validation.executor")
    if (
        set(executor) != {"jobs", "timeout_s"}
        or not isinstance(executor["jobs"], int)
        or isinstance(executor["jobs"], bool)
        or executor["jobs"] < 1
    ):
        _fail("compile_validation.executor", "invalid executor metadata")
    if (
        not isinstance(executor["timeout_s"], (int, float))
        or isinstance(executor["timeout_s"], bool)
        or executor["timeout_s"] <= 0
    ):
        _fail("compile_validation.executor", "invalid timeout")
    rows = value.get("rows")
    if not isinstance(rows, list) or len(rows) != 2 * count:
        _fail("compile_validation.rows", f"expected {2 * count} rows")
    seen: set[tuple[int, str]] = set()
    compiler_verified: set[str] = set()
    for raw in rows:
        raw = _mapping(raw, "compile_validation.rows")
        if set(raw) != COMPILE_ROW_FIELDS:
            _fail("compile_validation.rows", "row fields are not exact")
        index, turn = raw.get("index"), raw.get("turn")
        if not isinstance(index, int) or isinstance(index, bool) or not 0 <= index < count or turn not in ("A", "B"):
            _fail("compile_validation.rows", "invalid index/turn")
        key = (index, turn)
        if key in seen:
            _fail("compile_validation.rows", "duplicate index/turn")
        seen.add(key)
        expected = a_paths[index] if turn == "A" else b_paths[index]
        actual = trace_rows[index]["actual_input"]
        spec = command_specs.get(actual)
        if spec is None or raw.get("actual_input") != actual or raw.get("command_sha256") != spec["command_sha256"]:
            _fail(f"compile_validation.rows[{index},{turn}]", "compile-command binding mismatch")
        if (
            raw.get("resolution_basis") != spec["resolution_basis"]
            or raw.get("resolution_row_sha256") != spec["resolution_row_sha256"]
        ):
            _fail(f"compile_validation.rows[{index},{turn}]", "compile resolution binding mismatch")
        if raw.get("input_sha256") != _sha256(expected):
            _fail(f"compile_validation.rows[{index},{turn}]", "input binding mismatch")
        output_path = _safe_output(raw.get("object_path"), f"compile_validation.rows[{index},{turn}].object_path")
        expected_argv = _reconstruct_argv(spec, expected, output_path)
        if (
            not isinstance(raw.get("argv"), list)
            or not all(isinstance(item, str) for item in raw["argv"])
            or raw.get("argv") != expected_argv
            or raw.get("working_directory") != spec["directory"]
        ):
            _fail(f"compile_validation.rows[{index},{turn}]", "reconstructed argv or directory mismatch")
        compiler_path = raw.get("compiler_path")
        compiler_sha = raw.get("compiler_sha256")
        if compiler_path != spec["compiler_path"] or compiler_sha != spec["compiler_sha256"]:
            _fail(f"compile_validation.rows[{index},{turn}]", "compiler binding mismatch")
        if compiler_path not in compiler_verified:
            _bound_file(compiler_path, compiler_sha, f"compile_validation.rows[{index},{turn}].compiler")
            compiler_verified.add(compiler_path)
        if (
            not isinstance(raw.get("elapsed_ms"), (int, float))
            or isinstance(raw["elapsed_ms"], bool)
            or raw["elapsed_ms"] < 0
            or not isinstance(raw.get("exit_code"), int)
            or isinstance(raw["exit_code"], bool)
            or not isinstance(raw.get("timed_out"), bool)
        ):
            _fail(f"compile_validation.rows[{index},{turn}]", "invalid elapsed_ms")
        if not isinstance(raw.get("error"), str) or not isinstance(raw.get("stderr_first_diagnostic"), str):
            _fail(f"compile_validation.rows[{index},{turn}]", "invalid diagnostic fields")
        _sha(raw.get("stderr_sha256"), f"compile_validation.rows[{index},{turn}].stderr_sha256")
        _sha(raw.get("stdout_sha256"), f"compile_validation.rows[{index},{turn}].stdout_sha256")
        if raw.get("exit_code") != 0 or raw.get("timed_out") is not False:
            _fail(f"compile_validation.rows[{index},{turn}]", "not a definitive pass")
        _sha(raw.get("output_sha256"), f"compile_validation.rows[{index},{turn}].output_sha256")
    if len(seen) != 2 * count:
        _fail("compile_validation.rows", "missing index/turn")
    return value


def _validate_inputs(
    *,
    source_root: Path,
    source_commit: str,
    root_include: str,
    header_before: Path,
    header_after: Path,
    edit_diff: Path,
    trace_path: Path,
    compile_commands_path: Path,
    a_manifest_path: Path,
    b_manifest_path: Path,
    compile_validation_path: Path,
    count: int,
    expected_affected: int,
    expected_unaffected: int,
    provenance: Mapping[str, Any],
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    source_root = _directory(str(source_root), "source.root")
    if not isinstance(source_commit, str) or HEX40_RE.fullmatch(source_commit) is None:
        _fail("source.commit", "must be a full lowercase git commit")
    git = _git_worktree(source_root, source_commit)
    header_evidence = _source_header_evidence(source_root, source_commit, header_before)
    if not isinstance(provenance, Mapping) or provenance.get("mode") not in TRUE_PATH_MODES:
        _fail("provenance.mode", "must prove true-path binding")
    mode = provenance["mode"]
    expected_provenance_fields = {"edit_diff", "edited_header", "mode", "true_path_bind"}
    retained_path: Path | None = None
    if mode == "retained-true-path-revalidation":
        expected_provenance_fields.add("execution_receipt")
        receipt_binding = _mapping(provenance.get("execution_receipt"), "provenance.execution_receipt")
        if set(receipt_binding) != FILE_BIND_FIELDS:
            _fail("provenance.execution_receipt", "fields are not exact")
        retained_path = _bound_file(
            receipt_binding.get("path"), receipt_binding.get("sha256"), "provenance.execution_receipt"
        )
    if set(provenance) != expected_provenance_fields:
        _fail("provenance", "fields are not exact")
    stable: dict[Path, str] = {
        path: _sha256(path)
        for path in (
            header_before,
            header_after,
            edit_diff,
            trace_path,
            compile_commands_path,
            a_manifest_path,
            b_manifest_path,
            compile_validation_path,
        )
    }
    if retained_path is not None:
        stable[retained_path] = _sha256(retained_path)
    edit = _header_edit(header_before, header_after, edit_diff, root_include)
    try:
        header_before.relative_to(source_root)
    except ValueError:
        _fail("root_edit.path", "must be inside source.root")
    bind = provenance.get("true_path_bind")
    if not isinstance(bind, Mapping) or set(bind) != {"namespace_argv", "mount_argv", "target"}:
        _fail("provenance.true_path_bind", "is required")
    namespace = bind.get("namespace_argv")
    mount = bind.get("mount_argv")
    if not isinstance(namespace, list) or tuple(namespace) != NAMESPACE_ARGV:
        _fail("provenance.true_path_bind.namespace_argv", "does not prove private mount namespace")
    if not isinstance(mount, list) or mount != ["mount", "--bind", str(header_after), root_include]:
        _fail("provenance.true_path_bind.mount_argv", "does not prove mount --bind")
    if bind.get("target") != root_include or provenance.get("edited_header") != str(header_after):
        _fail("provenance.true_path_bind.target", "does not equal root header")
    trace_rows = _trace(trace_path, count)
    a_paths = _manifest(a_manifest_path, count, "turn_a_manifest")
    b_paths = _manifest(b_manifest_path, count, "turn_b_manifest")
    turn_a_root = _manifest_root(a_paths, trace_rows, "turn_a_manifest")
    turn_b_root = _manifest_root(b_paths, trace_rows, "turn_b_manifest")
    command_specs = _compile_commands(compile_commands_path, trace_rows, a_paths, True)
    for index, (trace, a_path, b_path) in enumerate(zip(trace_rows, a_paths, b_paths, strict=True)):
        if a_path == b_path:
            _fail(f"pair[{index}]", "A and B paths must differ")
    affected = 0
    inserted = int(edit["inserted_byte_hex"], 16)
    pair_rows: list[dict[str, Any]] = []
    for index, (a_path, b_path) in enumerate(zip(a_paths, b_paths, strict=True)):
        identical, changed = _same_or_single_insert(a_path, b_path, inserted)
        if identical:
            affected_flag = False
        elif changed:
            affected += 1
            affected_flag = True
        else:
            _fail(f"pair[{index}]", "B is neither identical A nor exact one-byte insertion")
        a_sha = _sha256(a_path)
        b_sha = _sha256(b_path)
        stable[a_path] = a_sha
        stable[b_path] = b_sha
        pair_rows.append(
            {
                "affected": affected_flag,
                "index": index,
                "path": trace_rows[index]["ii_relative"],
                "turn_a_sha256": a_sha,
                "turn_b_sha256": b_sha,
            }
        )
    if affected != expected_affected or count - affected != expected_unaffected:
        _fail("body_law", f"expected {expected_affected}/{expected_unaffected}, got {affected}/{count - affected}")
    compile_doc = _compile_receipt(
        compile_validation_path,
        count,
        source_root,
        source_commit,
        header_before,
        a_paths,
        b_paths,
        trace_rows,
        command_specs,
        _sha256(compile_commands_path),
    )
    retained_doc = None
    if retained_path is not None:
        retained_doc = _retained_receipt(
            retained_path,
            count,
            source_root,
            source_commit,
            header_before,
            header_after,
            a_paths,
            b_paths,
            trace_rows,
            command_specs,
            _sha256(compile_commands_path),
        )
        for retained_row in retained_doc["rows"]:
            stable[Path(retained_row["mountinfo_path"])] = _sha256(Path(retained_row["mountinfo_path"]))
            stable[Path(retained_row["stderr_path"])] = _sha256(Path(retained_row["stderr_path"]))
    selection = {
        "compile_commands_sha256": _sha256(compile_commands_path),
        "count": count,
        "rows": [
            {
                "actual_input": row["actual_input"],
                "command_sha256": command_specs[row["actual_input"]]["command_sha256"],
                "ii_relative": row["ii_relative"],
                "index": row["logical"],
                "resolution_basis": command_specs[row["actual_input"]]["resolution_basis"],
                "resolution_row_sha256": command_specs[row["actual_input"]]["resolution_row_sha256"],
                "turn_a_path": str(a_paths[index]),
                "turn_b_path": str(b_paths[index]),
            }
            for index, row in enumerate(trace_rows)
        ],
        "schema": TRACE_SCHEMA,
        "trace_sha256": _sha256(trace_path),
        "turn_a_root": str(turn_a_root),
        "turn_b_root": str(turn_b_root),
    }
    pair = {"pairs": pair_rows, "schema": PAIR_SCHEMA}
    for path, expected_sha in stable.items():
        if _sha256(path) != expected_sha:
            _fail("authority", f"input changed during validation: {path}")
    if _git_worktree(source_root, source_commit) != git:
        _fail("source.git", "worktree changed during validation")
    return edit, selection, pair, {
        "compile": compile_doc,
        "git": git,
        "header": header_evidence,
        "retained": retained_doc,
        "retained_path": retained_path,
    }


def build_root_header_authority(
    *,
    output_root: Path,
    source_root: Path,
    source_commit: str,
    root_include: str,
    header_before: Path,
    header_after: Path,
    edit_diff: Path,
    trace_path: Path,
    compile_commands_path: Path,
    turn_a_manifest: Path,
    turn_b_manifest: Path,
    compile_validation: Path,
    provenance: Mapping[str, Any],
    count: int = 1000,
    expected_affected: int = 761,
    expected_unaffected: int = 239,
    published_root: Path | None = None,
) -> dict[str, Any]:
    """Create an immutable root-header authority without overwriting output."""
    if output_root.exists() or output_root.is_symlink():
        raise RootHeaderAuthorityError(f"output already exists: {output_root}")
    output_root = output_root.absolute()
    published_root = (published_root or output_root).absolute()
    if (
        count != 1000
        or not isinstance(expected_affected, int)
        or isinstance(expected_affected, bool)
        or not isinstance(expected_unaffected, int)
        or isinstance(expected_unaffected, bool)
        or expected_affected < 1
        or expected_unaffected < 1
        or expected_affected + expected_unaffected != count
    ):
        _fail(
            "count",
            "root-header authority requires exactly 1000 TUs and a declared "
            "nonempty affected/unaffected body law",
        )
    edit, selection, pair, validation_meta = _validate_inputs(
        source_root=source_root,
        source_commit=source_commit,
        root_include=root_include,
        header_before=header_before,
        header_after=header_after,
        edit_diff=edit_diff,
        trace_path=trace_path,
        compile_commands_path=compile_commands_path,
        a_manifest_path=turn_a_manifest,
        b_manifest_path=turn_b_manifest,
        compile_validation_path=compile_validation,
        count=count,
        expected_affected=expected_affected,
        expected_unaffected=expected_unaffected,
        provenance=provenance,
    )
    parent = output_root.parent
    parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output_root.name}-", dir=parent))
    try:
        final_files = {
            "turn_a_manifest": published_root / "turnA.manifest",
            "turn_b_manifest": published_root / "turnB.manifest",
            "selection": published_root / "selection.json",
            "pair_index": published_root / "pair-index.json",
            "compile_validation": published_root / "compile-validation.json",
        }
        if validation_meta["retained_path"] is not None:
            final_files["retained_execution"] = published_root / "retained-execution.json"
            final_files["retained_mountinfo"] = published_root / "retained-execution.mountinfo"
            final_files["retained_stderr"] = published_root / "retained-execution.stderr"
        (temporary / "turnA.manifest").write_bytes(turn_a_manifest.read_bytes())
        (temporary / "turnB.manifest").write_bytes(turn_b_manifest.read_bytes())
        (temporary / "compile-validation.json").write_bytes(compile_validation.read_bytes())
        if validation_meta["retained_path"] is not None:
            retained_source = validation_meta["retained_path"]
            retained_value = _json(retained_source, "retained_execution")
            retained_value = dict(retained_value)
            retained_value["rows"] = [dict(row) for row in retained_value["rows"]]
            source_mountinfo = retained_source.with_suffix(".mountinfo")
            destination_mountinfo = temporary / "retained-execution.mountinfo"
            shutil.copytree(source_mountinfo, destination_mountinfo)
            source_stderr = retained_source.with_suffix(".stderr")
            destination_stderr = temporary / "retained-execution.stderr"
            shutil.copytree(source_stderr, destination_stderr)
            for row in retained_value["rows"]:
                row["mountinfo_path"] = str(
                    final_files["retained_mountinfo"] / Path(row["mountinfo_path"]).name
                )
                namespace = list(row["namespace_argv"])
                namespace_payload = json.loads(namespace[-1])
                namespace_payload["mountinfo_path"] = row["mountinfo_path"]
                namespace[-1] = canonical_bytes(namespace_payload).decode("utf-8")
                row["namespace_argv"] = namespace
                row["stderr_path"] = str(final_files["retained_stderr"] / Path(row["stderr_path"]).name)
            (temporary / "retained-execution.json").write_bytes(canonical_bytes(retained_value))
        (temporary / "selection.json").write_bytes(canonical_bytes(selection))
        (temporary / "pair-index.json").write_bytes(canonical_bytes(pair))
        authority_provenance = dict(provenance)
        if validation_meta["retained_path"] is not None:
            authority_provenance["execution_receipt"] = {
                "path": str(final_files["retained_execution"]),
                "sha256": _sha256(temporary / "retained-execution.json"),
            }
        authority = {
            "body_law": {"affected": expected_affected, "unaffected": expected_unaffected},
            "compile_validation": {
                "path": str(final_files["compile_validation"]),
                "sha256": _sha256(temporary / "compile-validation.json"),
                "turn_a_pass": count,
                "turn_b_pass": count,
            },
            "compile_commands": {
                "path": str(compile_commands_path),
                "sha256": _sha256(compile_commands_path),
            },
            "generation": {
                "generator": "farmharness.integration.firefox_root_header_authority",
                "source_commit": source_commit,
                "trace_sha256": _sha256(trace_path),
                "compile_commands_sha256": _sha256(compile_commands_path),
            },
            "pair_index": {
                "path": str(final_files["pair_index"]),
                "sha256": _sha256(temporary / "pair-index.json"),
            },
            "provenance": authority_provenance,
            "root_edit": edit,
            "schema": AUTHORITY_SCHEMA,
            "selection": {
                "path": str(final_files["selection"]),
                "sha256": _sha256(temporary / "selection.json"),
            },
            "source": {
                "commit": source_commit,
                "root": str(source_root),
                "git": validation_meta["git"],
                "header": validation_meta["header"],
            },
            "trace": {"path": str(trace_path), "sha256": _sha256(trace_path)},
            "turn_a_manifest": {
                "path": str(final_files["turn_a_manifest"]),
                "sha256": _sha256(temporary / "turnA.manifest"),
            },
            "turn_b_manifest": {
                "path": str(final_files["turn_b_manifest"]),
                "sha256": _sha256(temporary / "turnB.manifest"),
            },
            "tus": count,
        }
        (temporary / "authority.json").write_bytes(canonical_bytes(authority))
        _publish_directory_noreplace(temporary, output_root)
        temporary = Path()
    finally:
        if temporary != Path():
            shutil.rmtree(temporary, ignore_errors=True)
    return authority


def validate_root_header_authority(authority_path: Path) -> dict[str, Any]:
    """Independently revalidate a generated root-header authority bundle."""
    authority_path = _regular(str(authority_path), "authority")
    authority = _json(authority_path, "authority")
    if not isinstance(authority, dict) or authority.get("schema") != AUTHORITY_SCHEMA:
        _fail("authority.schema", f"expected {AUTHORITY_SCHEMA!r}")
    required = {
        "body_law", "compile_commands", "compile_validation", "pair_index",
        "generation", "provenance", "root_edit", "schema", "selection", "source", "trace",
        "turn_a_manifest", "turn_b_manifest", "tus",
    }
    if set(authority) != required:
        _fail("authority", "fields are not exact")
    body_law = authority["body_law"]
    body_law = _mapping(body_law, "body_law")
    count = authority["tus"]
    affected = body_law.get("affected")
    unaffected = body_law.get("unaffected")
    if (
        not isinstance(count, int)
        or isinstance(count, bool)
        or count < 1
        or not isinstance(affected, int)
        or isinstance(affected, bool)
        or not isinstance(unaffected, int)
        or isinstance(unaffected, bool)
        or affected < 0
        or unaffected < 0
        or affected + unaffected != count
    ):
        _fail("body_law", "counts are invalid")
    if count != 1000 or affected < 1 or unaffected < 1:
        _fail(
            "body_law",
            "root-header authority requires 1000 TUs and nonempty affected/unaffected sets",
        )
    root_edit = _mapping(authority["root_edit"], "root_edit")
    source = _mapping(authority["source"], "source")
    if set(root_edit) != ROOT_EDIT_FIELDS or set(source) != {"commit", "git", "header", "root"}:
        _fail("authority", "root_edit/source fields are not exact")
    generation = _mapping(authority["generation"], "generation")
    if set(generation) != {"compile_commands_sha256", "generator", "source_commit", "trace_sha256"}:
        _fail("generation", "fields are not exact")
    if generation.get("generator") != "farmharness.integration.firefox_root_header_authority":
        _fail("generation.generator", "unexpected generator")
    _sha(generation.get("trace_sha256"), "generation.trace_sha256")
    _sha(generation.get("compile_commands_sha256"), "generation.compile_commands_sha256")
    before = _regular(root_edit.get("path"), "root_edit.path")
    source_root = _directory(source.get("root"), "source.root")
    source_commit = source.get("commit")
    source_git = _mapping(source.get("git"), "source.git")
    source_header = _mapping(source.get("header"), "source.header")
    if set(source_header) != {"committed_sha256", "current_sha256", "path", "relative", "tracked"}:
        _fail("source.header", "fields are not exact")
    if set(source_git) != {"head", "status", "status_sha256"}:
        _fail("source.git", "fields are not exact")
    after_sha = _sha(root_edit.get("after_sha256"), "root_edit.after_sha256")
    diff_sha = _sha(root_edit.get("diff_sha256"), "root_edit.diff_sha256")
    # A generated bundle retains the edited header and diff paths in provenance.
    provenance = _mapping(authority["provenance"], "provenance")
    mode = provenance.get("mode")
    expected_provenance_fields = {"edit_diff", "edited_header", "mode", "true_path_bind"}
    if mode == "retained-true-path-revalidation":
        expected_provenance_fields.add("execution_receipt")
    if set(provenance) != expected_provenance_fields:
        _fail("provenance", "fields are not exact")
    after = _regular(provenance.get("edited_header"), "provenance.edited_header")
    diff = _regular(provenance.get("edit_diff"), "provenance.edit_diff")
    if _sha256(after) != after_sha or _sha256(diff) != diff_sha:
        _fail("provenance", "edited header or diff hash mismatch")
    if mode == "retained-true-path-revalidation":
        execution_binding = _mapping(provenance.get("execution_receipt"), "provenance.execution_receipt")
        if set(execution_binding) != FILE_BIND_FIELDS:
            _fail("provenance.execution_receipt", "fields are not exact")
        _bound_file(
            execution_binding.get("path"), execution_binding.get("sha256"), "provenance.execution_receipt"
        )
    compile_commands_doc = _mapping(authority["compile_commands"], "compile_commands")
    compile_validation_doc = _mapping(authority["compile_validation"], "compile_validation")
    pair_doc = _mapping(authority["pair_index"], "pair_index")
    selection_doc = _mapping(authority["selection"], "selection")
    trace_doc = _mapping(authority["trace"], "trace")
    a_doc = _mapping(authority["turn_a_manifest"], "turn_a_manifest")
    b_doc = _mapping(authority["turn_b_manifest"], "turn_b_manifest")
    if set(compile_commands_doc) != FILE_BIND_FIELDS or set(compile_validation_doc) != COMPILE_BIND_FIELDS:
        _fail("authority", "compile binding fields are not exact")
    if compile_validation_doc["turn_a_pass"] != count or compile_validation_doc["turn_b_pass"] != count:
        _fail("compile_validation", "turn pass counts are not exact")
    if set(pair_doc) != FILE_BIND_FIELDS or set(selection_doc) != FILE_BIND_FIELDS or set(trace_doc) != FILE_BIND_FIELDS:
        _fail("authority", "embedded document binding fields are not exact")
    if set(a_doc) != FILE_BIND_FIELDS or set(b_doc) != FILE_BIND_FIELDS:
        _fail("authority", "manifest binding fields are not exact")
    compile_commands = _bound_file(
        compile_commands_doc.get("path"),
        compile_commands_doc.get("sha256"),
        "compile_commands",
    )
    trace = _bound_file(trace_doc.get("path"), trace_doc.get("sha256"), "trace")
    a = _bound_file(a_doc.get("path"), a_doc.get("sha256"), "turn_a_manifest")
    b = _bound_file(b_doc.get("path"), b_doc.get("sha256"), "turn_b_manifest")
    selection = _bound_file(selection_doc.get("path"), selection_doc.get("sha256"), "selection")
    pair = _bound_file(pair_doc.get("path"), pair_doc.get("sha256"), "pair_index")
    compile_validation = _bound_file(compile_validation_doc.get("path"), compile_validation_doc.get("sha256"), "compile_validation")
    if generation["source_commit"] != source_commit:
        _fail("generation.source_commit", "does not match source commit")
    if generation["trace_sha256"] != _sha256(trace):
        _fail("generation.trace_sha256", "trace hash mismatch")
    if generation["compile_commands_sha256"] != _sha256(compile_commands):
        _fail("generation.compile_commands_sha256", "compile database hash mismatch")
    observed_git = _git_worktree(source_root, source_commit)
    if source_git != observed_git:
        _fail("source.git", "worktree evidence changed")
    observed_header = _source_header_evidence(source_root, source_commit, before)
    if source_header != observed_header:
        _fail("source.header", "tracked header evidence changed")
    edit, selected, pairs, _ = _validate_inputs(
        source_root=source_root,
        source_commit=source_commit,
        root_include=str(before),
        header_before=before,
        header_after=after,
        edit_diff=diff,
        trace_path=trace,
        compile_commands_path=compile_commands,
        a_manifest_path=a,
        b_manifest_path=b,
        compile_validation_path=compile_validation,
        count=count,
        expected_affected=affected,
        expected_unaffected=unaffected,
        provenance=provenance,
    )
    if _sha256(selection) != selection_doc["sha256"] or _sha256(pair) != pair_doc["sha256"]:
        _fail("authority", "embedded documents changed during validation")
    selection_value = _json(selection, "selection")
    pair_value = _json(pair, "pair_index")
    if not isinstance(selection_value, dict) or set(selection_value) != SELECTION_FIELDS:
        _fail("selection", "fields are not exact")
    rows = selection_value.get("rows")
    if not isinstance(rows, list) or any(
        not isinstance(row, dict) or set(row) != SELECTION_ROW_FIELDS for row in rows
    ):
        _fail("selection.rows", "fields are not exact")
    if not isinstance(pair_value, dict) or set(pair_value) != PAIR_FIELDS:
        _fail("pair_index", "fields are not exact")
    pair_rows_value = pair_value.get("pairs")
    if not isinstance(pair_rows_value, list) or any(
        not isinstance(row, dict) or set(row) != PAIR_ROW_FIELDS for row in pair_rows_value
    ):
        _fail("pair_index.pairs", "fields are not exact")
    if selection_value != selected or pair_value != pairs:
        _fail("authority", "selection or pair index contents do not match bodies")
    if dict(root_edit) != edit:
        _fail("root_edit", "recomputed root edit differs")
    return authority


def _safe_destination_directory(path: Path, subject: str) -> Path:
    path = Path(path).absolute()
    if path == Path("/") or ".." in path.parts or path.is_symlink() or path.exists():
        _fail(subject, "must be an absent absolute non-symlink directory path")
    return path


def capture_root_header_authority(
    *,
    output_root: Path,
    capture_receipt_path: Path,
    source_root: Path,
    source_commit: str,
    root_include: Path,
    header_before: Path,
    header_after: Path,
    edit_diff: Path,
    trace_path: Path,
    compile_commands_path: Path,
    turn_a_manifest: Path,
    turn_b_manifest: Path,
    count: int = 1000,
    expected_affected: int = 761,
    expected_unaffected: int = 239,
    timeout_s: float = 120.0,
    jobs: int = 4,
    invoke: Callable[[list[str], float], subprocess.CompletedProcess[bytes]] | None = None,
) -> dict[str, Any]:
    """Own and execute the complete production capture, never accepting receipts.

    The compile-validation and retained true-path receipts are created inside a
    fresh private staging directory and are the only inputs passed to the
    authority builder.  A failure leaves no authority output; low-level APIs
    remain separately available for bounded unit tests.
    """
    output_root = _safe_destination_directory(output_root, "capture.output")
    capture_receipt_path = Path(capture_receipt_path).absolute()
    if capture_receipt_path.is_symlink() or capture_receipt_path.exists():
        _fail("capture.receipt", "must be an absent non-symlink file")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output_root.name}-capture-", dir=output_root.parent))
    compile_receipt = staging / "compile-validation.json"
    retained_receipt = staging / "retained-execution.json"
    preserve_failure = False
    try:
        compile_document = run_compile_validation(
            source_root=source_root, source_commit=source_commit, root_include=root_include,
            trace_path=trace_path, compile_commands_path=compile_commands_path,
            turn_a_manifest=turn_a_manifest, turn_b_manifest=turn_b_manifest,
            output_path=compile_receipt, count=count, timeout_s=timeout_s, jobs=jobs,
        )
        failed_compile = (
            next(
                (
                    row
                    for row in compile_document["rows"]
                    if row.get("exit_code") != 0
                    or row.get("timed_out") is not False
                    or row.get("error") != ""
                    or row.get("output_sha256") is None
                ),
                None,
            )
            if isinstance(compile_document, Mapping)
            else None
        )
        if failed_compile is not None:
            _fail(
                "compile_validation.rows"
                f"[{failed_compile.get('index')},{failed_compile.get('turn')}]",
                "not a definitive pass",
            )
        run_retained_true_path_revalidation(
            source_root=source_root, source_commit=source_commit, root_include=root_include,
            header_after=header_after, trace_path=trace_path,
            compile_commands_path=compile_commands_path, turn_a_manifest=turn_a_manifest,
            turn_b_manifest=turn_b_manifest, output_path=retained_receipt,
            count=count, timeout_s=timeout_s, jobs=jobs, invoke=invoke,
        )
        provenance = {
            "edit_diff": str(edit_diff),
            "edited_header": str(header_after),
            "mode": "retained-true-path-revalidation",
            "true_path_bind": {
                "namespace_argv": list(NAMESPACE_ARGV),
                "mount_argv": ["mount", "--bind", str(header_after), str(root_include)],
                "target": str(root_include),
            },
            "execution_receipt": {
                "path": str(retained_receipt),
                "sha256": _sha256(retained_receipt),
            },
        }
        authority_tree = staging / "authority-tree"
        build_root_header_authority(
            output_root=authority_tree, source_root=source_root, source_commit=source_commit,
            root_include=str(root_include), header_before=header_before,
            header_after=header_after, edit_diff=edit_diff, trace_path=trace_path,
            compile_commands_path=compile_commands_path, turn_a_manifest=turn_a_manifest,
            turn_b_manifest=turn_b_manifest, compile_validation=compile_receipt,
            provenance=provenance, count=count,
            expected_affected=expected_affected,
            expected_unaffected=expected_unaffected,
            published_root=output_root,
        )
        capture_receipt = {
            "authority": {
                "path": str(output_root / "authority.json"),
                "sha256": _sha256(authority_tree / "authority.json"),
            },
            "compile_validation_sha256": _sha256(authority_tree / "compile-validation.json"),
            "count": count,
            "retained_execution_sha256": _sha256(authority_tree / "retained-execution.json"),
            "schema": CAPTURE_SCHEMA,
            "source_commit": source_commit,
            "status": "PASS",
        }
        _publish_directory_noreplace(authority_tree, output_root)
        validate_root_header_authority(output_root / "authority.json")
        _write_once_json(output_root / "capture-receipt.json", capture_receipt)
        if capture_receipt_path != output_root / "capture-receipt.json":
            _publish_json_noreplace(capture_receipt_path, capture_receipt)
        return capture_receipt
    except BaseException as exc:
        # The authority is only published after both producer stages succeed.
        # Preserve producer receipts as a terminal failure artifact without
        # exposing an authority or claiming success.
        if not output_root.exists() and staging.exists():
            failure_root = output_root.parent / f".{output_root.name}-failed"
            try:
                _publish_directory_noreplace(staging, failure_root)
                preserve_failure = True
                _write_once_json(
                    capture_receipt_path,
                    {
                        "error": f"{type(exc).__name__}: {exc}",
                        "failure_root": str(failure_root),
                        "schema": CAPTURE_SCHEMA,
                        "source_commit": source_commit,
                        "status": "FAIL",
                    },
                )
            except BaseException:
                # The original producer error remains authoritative; avoid
                # replacing it with evidence-publication noise.
                pass
        raise
    finally:
        if not preserve_failure:
            shutil.rmtree(staging, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--root-include", type=Path, required=True)
    parser.add_argument("--header-after", type=Path)
    parser.add_argument("--edit-diff", type=Path)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--turn-a", type=Path, required=True)
    parser.add_argument("--turn-b", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--expected-affected", type=int, default=761)
    parser.add_argument("--expected-unaffected", type=int, default=239)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--capture-receipt", type=Path)
    args = parser.parse_args(argv)
    if args.capture:
        if args.header_after is None or args.edit_diff is None or args.capture_receipt is None:
            parser.error("--capture requires --header-after, --edit-diff, and --capture-receipt")
        capture_root_header_authority(
            output_root=args.output,
            capture_receipt_path=args.capture_receipt,
            source_root=args.source_root,
            source_commit=args.source_commit,
            root_include=args.root_include,
            header_before=args.root_include,
            header_after=args.header_after,
            edit_diff=args.edit_diff,
            trace_path=args.trace,
            compile_commands_path=args.compile_commands,
            turn_a_manifest=args.turn_a,
            turn_b_manifest=args.turn_b,
            count=args.count,
            expected_affected=args.expected_affected,
            expected_unaffected=args.expected_unaffected,
            timeout_s=args.timeout,
            jobs=args.jobs,
        )
        return 0
    run_compile_validation(
        source_root=args.source_root,
        source_commit=args.source_commit,
        root_include=args.root_include,
        trace_path=args.trace,
        compile_commands_path=args.compile_commands,
        turn_a_manifest=args.turn_a,
        turn_b_manifest=args.turn_b,
        output_path=args.output,
        count=args.count,
        timeout_s=args.timeout,
        jobs=args.jobs,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
