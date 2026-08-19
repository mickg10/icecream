#!/usr/bin/env python3
"""Build the corrected Firefox job trace used by distribution experiments.

Mozilla's CompileDB has one row for each constituent source of a unified build,
but many rows invoke the same ``Unified_cpp_*.cpp`` operand.  The original corpus
capture keyed /dev/null outputs by CompileDB's descriptive ``file`` field and
therefore retained the same real compiler input repeatedly.  This tool collapses
rows by the command that was actually executed, chooses one captured .ii for each
real invocation, and assigns the retained compile-time model to every job.

The provisional ``firefox-compile-v1-size-linear`` model is piecewise-linear in .ii size over
the measurements in firefox_compile_samples.tsv.  Rows labelled ``early-error``
are retained because the experiment owner explicitly selected the distribution
we have now; they are called out in provenance and must not be relabelled as a
full successful-build timing census.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


CXX_SUFFIXES = (".cc", ".cpp", ".cxx", ".c++", ".C", ".CC")
MODEL = "firefox-compile-v1-size-linear"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def command_arguments(entry: dict[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if arguments is not None:
        if not isinstance(arguments, list) or not all(
            isinstance(x, str) for x in arguments
        ):
            raise ValueError("CompileDB arguments is not a string array")
        return list(arguments)
    command = entry.get("command")
    if not isinstance(command, str):
        raise ValueError("CompileDB row has neither arguments nor command")
    return shlex.split(command)


def absolute(path: str, directory: str) -> str:
    return os.path.normpath(
        path if os.path.isabs(path) else os.path.join(directory, path)
    )


def actual_input(arguments: list[str], directory: str) -> tuple[int, str]:
    candidates = [
        index
        for index, value in enumerate(arguments[1:], 1)
        if not value.startswith("-") and value.endswith(CXX_SUFFIXES)
    ]
    if not candidates:
        raise ValueError("compile command has no C++ input operand")
    # CompileDB rows describe one compile action.  A few options may themselves
    # name a C++-suffixed file, while the final such operand is the action input.
    index = candidates[-1]
    return index, absolute(arguments[index], directory)


def compile_identity(directory: str, arguments: Iterable[str]) -> str:
    digest = hashlib.sha256(b"firefox-real-compile-v1\0")
    digest.update(os.path.normpath(directory).encode())
    digest.update(b"\0")
    for argument in arguments:
        digest.update(argument.encode())
        digest.update(b"\0")
    return digest.hexdigest()


@dataclass(frozen=True)
class CompileRow:
    directory: str
    file: str
    arguments: tuple[str, ...]
    input_index: int
    actual_input: str
    identity: str


@dataclass
class Job:
    first_manifest_index: int
    identity: str
    actual_input: str
    representative: Path
    duplicate_rows: int = 1
    compile_ns: int = 0


def load_compile_rows(path: Path) -> dict[str, list[CompileRow]]:
    raw = json.loads(path.read_text())
    if not isinstance(raw, list):
        raise ValueError("CompileDB root is not an array")
    result: dict[str, list[CompileRow]] = {}
    for entry in raw:
        if not isinstance(entry, dict):
            raise ValueError("CompileDB row is not an object")
        file_value = entry.get("file")
        directory_value = entry.get("directory")
        if not isinstance(file_value, str) or not isinstance(directory_value, str):
            raise ValueError("CompileDB row lacks file/directory strings")
        file_value = absolute(file_value, directory_value)
        if not file_value.endswith(CXX_SUFFIXES):
            continue
        arguments = command_arguments(entry)
        input_index, input_value = actual_input(arguments, directory_value)
        row = CompileRow(
            os.path.normpath(directory_value),
            file_value,
            tuple(arguments),
            input_index,
            input_value,
            compile_identity(directory_value, arguments),
        )
        result.setdefault(file_value, []).append(row)
    return result


def source_for_ii(path: Path, corpus_root: Path) -> str:
    try:
        relative = path.relative_to(corpus_root)
    except ValueError as error:
        raise ValueError(f"{path} is outside corpus root {corpus_root}") from error
    text = str(relative)
    if not text.endswith(".ii"):
        raise ValueError(f"manifest path is not .ii: {path}")
    # preprocess_corpus.py maps an absolute source /a/b.cpp to
    # CORPUS/a/b.cpp.ii.  Reverse that mapping here.
    return os.path.normpath("/" + text[:-3])


def first_source_marker(path: Path) -> str:
    """Return the first real source operand recorded by clang's line markers."""
    marker = re.compile(r'^#\s+1\s+"([^"]+)"')
    with path.open(errors="replace") as source:
        for _ in range(16):
            line = source.readline()
            if not line:
                break
            match = marker.match(line)
            if match and not match.group(1).startswith("<"):
                return os.path.normpath(match.group(1))
    raise ValueError(f"captured .ii has no initial source marker: {path}")


def select_compile_row(path: Path, rows: list[CompileRow]) -> tuple[CompileRow, int]:
    """Select the CompileDB variant that produced a colliding corpus path.

    preprocess_corpus.py used the descriptive CompileDB `file` to name Firefox
    outputs.  A small set of source files appears in several build targets, so
    those commands collided on one path.  The .ii's first clang line marker tells
    us which actual operand won the write.  If several variants compile that same
    operand with different flags, the retained payload cannot distinguish them;
    choose the first and report the other identities as dropped provenance.
    """
    identities = {row.identity for row in rows}
    if len(identities) == 1:
        return rows[0], 0
    marker = first_source_marker(path)
    matches = []
    for row in rows:
        operand = os.path.normpath(row.arguments[row.input_index])
        if marker == operand or marker == row.actual_input:
            matches.append(row)
    if not matches:
        by_basename = [
            row
            for row in rows
            if os.path.basename(row.actual_input) == os.path.basename(marker)
        ]
        if len({row.actual_input for row in by_basename}) == 1:
            matches = by_basename
    if not matches:
        raise ValueError(
            f"cannot match first source marker {marker!r} to CompileDB variants for {path}"
        )
    selected = matches[0]
    return selected, len(identities - {selected.identity})


def load_samples(path: Path) -> tuple[list[tuple[int, int]], dict[str, int]]:
    points: list[tuple[int, int]] = []
    status: dict[str, int] = {}
    with path.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        required = {"ii_bytes", "wall_ns", "return_code", "status"}
        if reader.fieldnames is None or required - set(reader.fieldnames):
            raise ValueError("compile sample table lacks required columns")
        for row in reader:
            size = int(row["ii_bytes"])
            duration = int(row["wall_ns"])
            if size <= 0 or duration <= 0:
                raise ValueError("compile sample has a non-positive size/duration")
            points.append((size, duration))
            label = row["status"]
            status[label] = status.get(label, 0) + 1
    points.sort()
    if len(points) < 2 or len({size for size, _ in points}) != len(points):
        raise ValueError("compile sample sizes are not unique")
    if any(right[1] < left[1] for left, right in zip(points, points[1:])):
        raise ValueError("compile sample duration is not monotonic by size")
    return points, status


def interpolate(points: list[tuple[int, int]], size: int) -> int:
    if size <= points[0][0]:
        return points[0][1]
    if size >= points[-1][0]:
        return points[-1][1]
    for left, right in zip(points, points[1:]):
        if size > right[0]:
            continue
        width = right[0] - left[0]
        offset = size - left[0]
        delta = right[1] - left[1]
        return left[1] + (delta * offset + width // 2) // width
    raise AssertionError("interpolation interval not found")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--corpus-root", type=Path, required=True)
    parser.add_argument(
        "--samples",
        type=Path,
        default=Path(__file__).with_name("firefox_compile_samples.tsv"),
    )
    parser.add_argument("--out-trace", type=Path, required=True)
    parser.add_argument("--out-summary", type=Path, required=True)
    args = parser.parse_args()

    corpus_root = args.corpus_root.resolve()
    compile_rows = load_compile_rows(args.compile_commands)
    manifest = [Path(line) for line in args.manifest.read_text().splitlines() if line]
    if not manifest:
        raise ValueError("manifest is empty")

    jobs_by_identity: dict[str, Job] = {}
    ambiguous_sources = 0
    ambiguous_compile_variants_dropped = 0
    logical_bytes = 0
    for index, ii_path in enumerate(manifest):
        ii_path = ii_path.resolve()
        if not ii_path.is_file():
            raise ValueError(f"manifest path does not exist: {ii_path}")
        logical_bytes += ii_path.stat().st_size
        source = source_for_ii(ii_path, corpus_root)
        rows = compile_rows.get(source)
        if not rows:
            raise ValueError(f"no CompileDB row for captured source {source}")
        identities = {row.identity for row in rows}
        if len(identities) != 1:
            ambiguous_sources += 1
        row, dropped = select_compile_row(ii_path, rows)
        ambiguous_compile_variants_dropped += dropped
        existing = jobs_by_identity.get(row.identity)
        if existing is None:
            jobs_by_identity[row.identity] = Job(
                index, row.identity, row.actual_input, ii_path
            )
        else:
            if existing.representative.stat().st_size != ii_path.stat().st_size:
                raise ValueError(
                    "rows with one compile identity produced different .ii sizes: "
                    f"{existing.representative} and {ii_path}"
                )
            existing.duplicate_rows += 1

    jobs = sorted(jobs_by_identity.values(), key=lambda job: job.first_manifest_index)
    points, sample_status = load_samples(args.samples)
    for job in jobs:
        job.compile_ns = interpolate(points, job.representative.stat().st_size)

    args.out_trace.parent.mkdir(parents=True, exist_ok=True)
    with args.out_trace.open("w", newline="") as output:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow(
            [
                "logical",
                "job_id",
                "ii_relative",
                "actual_input",
                "raw_bytes",
                "compile_ns",
                "duplicate_rows",
                "compile_model",
            ]
        )
        for logical, job in enumerate(jobs):
            writer.writerow(
                [
                    logical,
                    job.identity,
                    job.representative.relative_to(corpus_root),
                    job.actual_input,
                    job.representative.stat().st_size,
                    job.compile_ns,
                    job.duplicate_rows,
                    MODEL,
                ]
            )

    retained_bytes = sum(job.representative.stat().st_size for job in jobs)
    durations = sorted(job.compile_ns for job in jobs)

    def percentile(numerator: int, denominator: int = 100) -> int:
        index = (len(durations) - 1) * numerator // denominator
        return durations[index]

    summary = {
        "schema": "firefox-corrected-workload-v1",
        "compile_model": MODEL,
        "manifest": str(args.manifest),
        "manifest_sha256": sha256(args.manifest),
        "compile_commands": str(args.compile_commands),
        "compile_commands_sha256": sha256(args.compile_commands),
        "samples": str(args.samples),
        "samples_sha256": sha256(args.samples),
        "sample_status": sample_status,
        "manifest_rows": len(manifest),
        "real_compile_jobs": len(jobs),
        "duplicate_rows_removed": len(manifest) - len(jobs),
        "logical_bytes_before": logical_bytes,
        "logical_bytes_after": retained_bytes,
        "logical_byte_ratio_before_over_after": logical_bytes / retained_bytes,
        "ambiguous_sources": ambiguous_sources,
        "ambiguous_compile_variants_dropped": ambiguous_compile_variants_dropped,
        "compile_ns": {
            "min": durations[0],
            "p10": percentile(10),
            "p25": percentile(25),
            "p50": percentile(50),
            "p75": percentile(75),
            "p90": percentile(90),
            "p95": percentile(95),
            "p99": percentile(99),
            "max": durations[-1],
            "sum": sum(durations),
        },
        "trace": str(args.out_trace),
    }
    args.out_summary.parent.mkdir(parents=True, exist_ok=True)
    args.out_summary.write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
