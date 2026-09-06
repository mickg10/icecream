#!/usr/bin/env python3
"""Build and compile-validate a deterministic Firefox A/B corpus authority."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

try:
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from schema_validation import canonical_bytes


AUTHORITY_SCHEMA = "icefarm-firefox-corpus-authority-v1"
PAIR_INDEX_SCHEMA = "icefarm-firefox-pair-index-v1"
MUTATION_BEFORE = b'mozalloc_abort("alloc overflow")'
MUTATION_AFTER = b'mozalloc_abort("Xalloc overflow")'


class FirefoxCorpusAuthorityError(RuntimeError):
    """The requested authority cannot be built without weakening its rules."""


@dataclass(frozen=True)
class TraceEntry:
    logical: int
    relative: PurePosixPath
    source: Path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _atomic_write(path: Path, payload: bytes) -> None:
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    if temporary.exists():
        raise FirefoxCorpusAuthorityError(f"temporary output already exists: {temporary}")
    try:
        temporary.write_bytes(payload)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _safe_relative(value: str, subject: str) -> PurePosixPath:
    relative = PurePosixPath(value)
    if (
        relative.is_absolute()
        or str(relative) in ("", ".")
        or ".." in relative.parts
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise FirefoxCorpusAuthorityError(f"{subject} is not a safe relative path")
    return relative


def load_corrected_trace(path: Path, corpus_root: Path) -> list[TraceEntry]:
    result: list[TraceEntry] = []
    seen: set[PurePosixPath] = set()
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        if reader.fieldnames is None or not {"logical", "ii_relative"} <= set(
            reader.fieldnames
        ):
            raise FirefoxCorpusAuthorityError("corrected trace lacks logical/ii_relative")
        for expected, row in enumerate(reader):
            logical = int(row["logical"])
            if logical != expected:
                raise FirefoxCorpusAuthorityError(
                    f"corrected trace logical index {logical} is not contiguous at {expected}"
                )
            relative = _safe_relative(row["ii_relative"], f"trace row {expected}")
            if relative in seen:
                raise FirefoxCorpusAuthorityError(
                    f"corrected trace repeats path {relative}"
                )
            source = corpus_root.joinpath(*relative.parts)
            if not source.is_file() or source.is_symlink():
                raise FirefoxCorpusAuthorityError(
                    f"corrected trace source is not a regular file: {source}"
                )
            seen.add(relative)
            result.append(TraceEntry(logical, relative, source))
    if not result:
        raise FirefoxCorpusAuthorityError("corrected trace is empty")
    return result


def load_compile_passes(paths: Iterable[Path]) -> dict[Path, dict[str, Any]]:
    result: dict[Path, dict[str, Any]] = {}
    for audit_path in paths:
        value = json.loads(audit_path.read_text(encoding="utf-8"))
        if not isinstance(value, list):
            raise FirefoxCorpusAuthorityError(f"audit rows are not an array: {audit_path}")
        for index, row in enumerate(value):
            if not isinstance(row, dict) or not isinstance(row.get("path"), str):
                raise FirefoxCorpusAuthorityError(
                    f"audit {audit_path} row {index} lacks a path"
                )
            source = Path(row["path"])
            if source in result:
                raise FirefoxCorpusAuthorityError(
                    f"compile audit repeats source {source}"
                )
            passed = (
                row.get("exit_code") == 0
                and row.get("timed_out") is False
                and isinstance(row.get("output_sha256"), str)
                and len(row["output_sha256"]) == 64
            )
            result[source] = {"audit": str(audit_path), "passed": passed, "row": row}
    return result


def select_compile_valid(
    trace: Iterable[TraceEntry], audits: dict[Path, dict[str, Any]], count: int
) -> list[TraceEntry]:
    if count < 1:
        raise FirefoxCorpusAuthorityError("selected TU count must be positive")
    selected = [entry for entry in trace if audits.get(entry.source, {}).get("passed")]
    if len(selected) < count:
        raise FirefoxCorpusAuthorityError(
            f"only {len(selected)} corrected trace entries have compile-pass evidence; "
            f"need {count}"
        )
    return selected[:count]


def _normalized_suffix(relative: PurePosixPath, marker: PurePosixPath) -> PurePosixPath:
    parts = relative.parts
    marker_parts = PurePosixPath(str(marker).lstrip("/")).parts
    if not marker_parts:
        raise FirefoxCorpusAuthorityError("authority marker is empty")
    for index in range(len(parts) - len(marker_parts) + 1):
        if parts[index : index + len(marker_parts)] == marker_parts:
            suffix = PurePosixPath(*parts[index + len(marker_parts) :])
            if str(suffix) in ("", "."):
                break
            return suffix
    raise FirefoxCorpusAuthorityError(
        f"trace path {relative} does not contain authority marker {marker}"
    )


def transform_turn_b(source: Path, target: Path) -> bool:
    payload = source.read_bytes()
    occurrences = payload.count(MUTATION_BEFORE)
    if occurrences > 1:
        raise FirefoxCorpusAuthorityError(
            f"Turn-B mutation occurs {occurrences} times in {source}"
        )
    transformed = payload.replace(MUTATION_BEFORE, MUTATION_AFTER, 1)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(transformed)
    return occurrences == 1


def _compile_one(
    *,
    turn: str,
    index: int,
    source: Path,
    expected_source_sha256: str,
    compiler: Path,
    compiler_arguments: tuple[str, ...],
    object_root: Path,
    timeout_s: int,
) -> dict[str, Any]:
    observed_source_sha256 = _sha256(source)
    if observed_source_sha256 != expected_source_sha256:
        return {
            "elapsed_ms": 0.0,
            "error": "source-drift-before-compile",
            "exit_code": 125,
            "index": index,
            "input_sha256": observed_source_sha256,
            "output_sha256": None,
            "path": str(source),
            "stderr_first_diagnostic": "",
            "stderr_sha256": hashlib.sha256(b"").hexdigest(),
            "stdout_sha256": hashlib.sha256(b"").hexdigest(),
            "timed_out": False,
            "turn": turn,
        }
    output = object_root / f"{turn}-{index:04d}.o"
    started = time.monotonic_ns()
    timed_out = False
    error = ""
    stdout = b""
    stderr = b""
    try:
        process = subprocess.run(
            (
                str(compiler),
                *compiler_arguments,
                "-c",
                str(source),
                "-o",
                str(output),
            ),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
        )
        return_code = process.returncode
        stdout = process.stdout
        stderr = process.stderr
    except subprocess.TimeoutExpired as exc:
        return_code = 124
        timed_out = True
        error = "timeout"
        stdout = exc.stdout or b""
        stderr = exc.stderr or b""
    except OSError as exc:
        return_code = 125
        error = f"{type(exc).__name__}: {exc}"
    elapsed_ms = round((time.monotonic_ns() - started) / 1_000_000, 3)
    passed = return_code == 0 and not timed_out and output.is_file()
    output_sha256 = _sha256(output) if passed else None
    output.unlink(missing_ok=True)
    first_diagnostic = next(
        (
            line.strip()[:1000]
            for line in stderr.decode("utf-8", "replace").splitlines()
            if line.strip()
        ),
        "",
    )
    return {
        "elapsed_ms": elapsed_ms,
        "error": error,
        "exit_code": return_code,
        "index": index,
        "input_sha256": observed_source_sha256,
        "output_sha256": output_sha256,
        "path": str(source),
        "stderr_first_diagnostic": first_diagnostic,
        "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
        "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
        "timed_out": timed_out,
        "turn": turn,
    }


def build_firefox_authority(
    *,
    trace_path: Path,
    corpus_root: Path,
    audit_rows: tuple[Path, ...],
    output_root: Path,
    authority_marker: PurePosixPath,
    compiler: Path,
    compiler_arguments: tuple[str, ...],
    count: int = 1000,
    jobs: int = 12,
    timeout_s: int = 1800,
) -> dict[str, Any]:
    if output_root.exists():
        raise FirefoxCorpusAuthorityError(f"authority output already exists: {output_root}")
    if not compiler.is_file() or compiler.is_symlink():
        raise FirefoxCorpusAuthorityError(f"compiler is not a regular file: {compiler}")
    if jobs < 1 or timeout_s < 1:
        raise FirefoxCorpusAuthorityError("jobs and timeout must be positive")
    trace = load_corrected_trace(trace_path, corpus_root)
    audits = load_compile_passes(audit_rows)
    selected = select_compile_valid(trace, audits, count)
    output_root.mkdir(parents=True, mode=0o755)
    turn_b_root = output_root / "turnB"
    object_root = output_root / ".objects"
    object_root.mkdir()
    selected_rows: list[dict[str, Any]] = []
    turn_a_manifest: list[str] = []
    turn_b_manifest: list[str] = []
    pair_rows: list[dict[str, Any]] = []
    changed = 0
    try:
        for index, entry in enumerate(selected):
            suffix = _normalized_suffix(entry.relative, authority_marker)
            target = turn_b_root.joinpath(*entry.relative.parts)
            affected = transform_turn_b(entry.source, target)
            changed += int(affected)
            turn_a_sha256 = _sha256(entry.source)
            turn_b_sha256 = _sha256(target)
            selected_rows.append(
                {
                    "audit": audits[entry.source]["audit"],
                    "corrected_trace_logical": entry.logical,
                    "index": index,
                    "relative": str(entry.relative),
                    "turn_a_path": str(entry.source),
                    "turn_a_sha256": turn_a_sha256,
                    "turn_b_path": str(target),
                    "turn_b_sha256": turn_b_sha256,
                }
            )
            pair_rows.append(
                {
                    "affected": affected,
                    "index": index,
                    "path": str(suffix),
                    "turn_a_sha256": turn_a_sha256,
                    "turn_b_sha256": turn_b_sha256,
                }
            )
            turn_a_manifest.append(str(entry.source))
            turn_b_manifest.append(str(target))
        if changed * 2 < count:
            raise FirefoxCorpusAuthorityError(
                f"Turn-B root edit affects only {changed}/{count} selected TUs"
            )
        a_manifest_path = output_root / "turnA.manifest"
        b_manifest_path = output_root / "turnB.manifest"
        _atomic_write(a_manifest_path, ("\n".join(turn_a_manifest) + "\n").encode())
        _atomic_write(b_manifest_path, ("\n".join(turn_b_manifest) + "\n").encode())
        normalized_payload = (
            "\n".join(row["path"] for row in pair_rows) + "\n"
        ).encode()
        pair_index = {"pairs": pair_rows, "schema": PAIR_INDEX_SCHEMA}
        pair_index_path = output_root / "pair-index.json"
        _atomic_write(pair_index_path, canonical_bytes(pair_index))
        selected_path = output_root / "selection.json"
        _atomic_write(
            selected_path,
            canonical_bytes(
                {
                    "count": count,
                    "rows": selected_rows,
                    "schema": "icefarm-firefox-selection-v1",
                }
            ),
        )

        work: list[tuple[str, int, Path, str]] = []
        for row in selected_rows:
            work.append(("A", row["index"], Path(row["turn_a_path"]), row["turn_a_sha256"]))
            work.append(("B", row["index"], Path(row["turn_b_path"]), row["turn_b_sha256"]))
        validation_rows: list[dict[str, Any]] = []
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = [
                pool.submit(
                    _compile_one,
                    turn=turn,
                    index=index,
                    source=source,
                    expected_source_sha256=digest,
                    compiler=compiler,
                    compiler_arguments=compiler_arguments,
                    object_root=object_root,
                    timeout_s=timeout_s,
                )
                for turn, index, source, digest in work
            ]
            for future in as_completed(futures):
                validation_rows.append(future.result())
        validation_rows.sort(key=lambda row: (row["index"], row["turn"]))
        validation_path = output_root / "compile-validation.json"
        _atomic_write(
            validation_path,
            canonical_bytes(
                {
                    "compiler": str(compiler),
                    "compiler_arguments": list(compiler_arguments),
                    "compiler_sha256": _sha256(compiler),
                    "jobs": jobs,
                    "rows": validation_rows,
                    "schema": "icefarm-firefox-compile-validation-v1",
                    "timeout_s": timeout_s,
                }
            ),
        )
        failures = [
            row
            for row in validation_rows
            if row["exit_code"] != 0
            or row["timed_out"]
            or not isinstance(row["output_sha256"], str)
            or len(row["output_sha256"]) != 64
        ]
        if failures:
            raise FirefoxCorpusAuthorityError(
                f"compile validation failed for {len(failures)}/{len(validation_rows)} inputs; "
                f"evidence retained at {validation_path}"
            )
        authority = {
            "compile_validation": {
                "path": str(validation_path),
                "sha256": _sha256(validation_path),
                "turn_a_pass": count,
                "turn_b_pass": count,
            },
            "compiler": {
                "arguments": list(compiler_arguments),
                "path": str(compiler),
                "sha256": _sha256(compiler),
            },
            "corrected_trace": {
                "path": str(trace_path),
                "sha256": _sha256(trace_path),
            },
            "input_audits": [
                {"path": str(path), "sha256": _sha256(path)} for path in audit_rows
            ],
            "mutation": {
                "affected": changed,
                "after": MUTATION_AFTER.decode("ascii"),
                "before": MUTATION_BEFORE.decode("ascii"),
                "unaffected": count - changed,
            },
            "normalized_manifest_sha256": hashlib.sha256(normalized_payload).hexdigest(),
            "pair_index": {
                "path": str(pair_index_path),
                "sha256": _sha256(pair_index_path),
            },
            "schema": AUTHORITY_SCHEMA,
            "selection": {"path": str(selected_path), "sha256": _sha256(selected_path)},
            "turn_a_manifest": {
                "path": str(a_manifest_path),
                "sha256": _sha256(a_manifest_path),
            },
            "turn_b_manifest": {
                "path": str(b_manifest_path),
                "sha256": _sha256(b_manifest_path),
            },
            "tus": count,
        }
        authority_path = output_root / "authority.json"
        _atomic_write(authority_path, canonical_bytes(authority))
        return {**authority, "authority_path": str(authority_path), "authority_sha256": _sha256(authority_path)}
    finally:
        shutil.rmtree(object_root, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--corpus-root", type=Path, required=True)
    parser.add_argument("--audit-rows", type=Path, action="append", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--authority-marker", type=PurePosixPath, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--compiler-argument", action="append", default=[])
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--timeout-s", type=int, default=1800)
    args = parser.parse_args(argv)
    result = build_firefox_authority(
        trace_path=args.trace.resolve(),
        corpus_root=args.corpus_root.resolve(),
        audit_rows=tuple(path.resolve() for path in args.audit_rows),
        output_root=args.output_root.resolve(),
        authority_marker=args.authority_marker,
        compiler=args.compiler.resolve(),
        compiler_arguments=tuple(args.compiler_argument),
        count=args.count,
        jobs=args.jobs,
        timeout_s=args.timeout_s,
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
