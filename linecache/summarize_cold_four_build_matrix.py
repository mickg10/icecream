#!/usr/bin/env python3
"""Render the exact cold P25-P29 retained-state four-build matrix."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from run_cold_four_build_matrix import BUILD_REPETITIONS, COLD_SCHEMAS
from run_complete_tu_matrix import CORPORA, sha256
from summarize_complete_tu_matrix import historical_endpoints, load_curve


DEFAULT_CHECKPOINTS = (50, 100, 150, 200, 250, 300)
ONE_GBIT_BYTES_PER_SECOND = 125_000_000
DECIMAL_MB = 1_000_000
# This experiment starts with no installed pre-shared package.  Keep the
# one-time package lane explicit so build-wire and installation accounting
# cannot be conflated when this table is compared with later S-package rows.
NO_SHARED_S_TRANSFER_BYTES = 0


def validate_repeated_curve(curve: list[dict], repetitions: int = BUILD_REPETITIONS) -> int:
    if len(curve) % repetitions:
        raise ValueError(
            f"curve has {len(curve)} rows, not a multiple of {repetitions} builds"
        )
    tus_per_build = len(curve) // repetitions
    if not tus_per_build:
        raise ValueError("curve has no TUs per build")
    expected_raw = [int(row["raw_bytes"]) for row in curve[:tus_per_build]]
    cumulative_raw = cumulative_wire = 0
    for ordinal, row in enumerate(curve, 1):
        if int(row["tu"]) != ordinal or row["exact"] != "true":
            raise ValueError(f"invalid curve row {ordinal}")
        expected = expected_raw[(ordinal - 1) % tus_per_build]
        if int(row["raw_bytes"]) != expected:
            raise ValueError(f"raw TU sequence differs at row {ordinal}")
        cumulative_raw += expected
        cumulative_wire += int(row["wire_bytes"])
        if int(row["cumulative_raw_bytes"]) != cumulative_raw:
            raise ValueError(f"cumulative raw bytes differ at row {ordinal}")
        if int(row["cumulative_wire_bytes"]) != cumulative_wire:
            raise ValueError(f"cumulative wire bytes differ at row {ordinal}")
    return tus_per_build


def curve_point(curve: list[dict], ordinal: int) -> dict:
    if ordinal <= 0 or ordinal > len(curve):
        raise ValueError(f"curve point {ordinal} is out of range")
    row = curve[ordinal - 1]
    raw = int(row["cumulative_raw_bytes"])
    wire = int(row["cumulative_wire_bytes"])
    return {
        "tu": ordinal,
        "raw_bytes": raw,
        "wire_bytes": wire,
        "decimal_mb": wire / DECIMAL_MB,
        "seconds_1gbit": wire / ONE_GBIT_BYTES_PER_SECOND,
        "ratio": raw / wire,
    }


def make_rows(
    run_root: Path,
    repository: Path,
    checkpoints: tuple[int, ...],
) -> list[dict]:
    expected = historical_endpoints(repository)
    rows = []
    for project, _ in CORPORA:
        for schema in COLD_SCHEMAS:
            curve_path = run_root / schema["schema"] / f"{project}.curve.tsv"
            log_path = run_root / schema["schema"] / f"{project}.log"
            curve = load_curve(curve_path)
            if not log_path.is_file():
                raise FileNotFoundError(log_path)
            tus_per_build = validate_repeated_curve(curve)
            first = curve_point(curve, tus_per_build)
            fourth = curve_point(curve, len(curve))
            key = (project, schema["stage"], "cold")
            if first["wire_bytes"] != expected[key]:
                raise ValueError(
                    f"{schema['schema']}/{project}: first-build wire "
                    f"{first['wire_bytes']} differs from retained {expected[key]}"
                )

            build_endpoints = [curve_point(curve, tus_per_build * build) for build in range(1, 5)]
            previous_wire = 0
            build_wire = []
            for endpoint in build_endpoints:
                build_wire.append(endpoint["wire_bytes"] - previous_wire)
                previous_wire = endpoint["wire_bytes"]

            row = {
                "project": project,
                "schema": schema["schema"],
                "stage": schema["stage"],
                "receiver_initial_state": "cold",
                "shared_package": "none",
                "s_one_time_transfer_bytes": NO_SHARED_S_TRANSFER_BYTES,
                "s_one_time_transfer_decimal_mb": 0.0,
                "s_one_time_transfer_seconds_1gbit": 0.0,
                "description": schema["description"],
                "build_repetitions": BUILD_REPETITIONS,
                "tus_per_build": tus_per_build,
                "total_tus": len(curve),
                "raw_bytes_per_build": first["raw_bytes"],
                "curve_sha256": sha256(curve_path),
                "log_sha256": sha256(log_path),
            }
            for checkpoint in checkpoints:
                prefix = f"tu{checkpoint}"
                if checkpoint > len(curve):
                    row[f"{prefix}_wire_bytes"] = ""
                    row[f"{prefix}_decimal_mb"] = ""
                    row[f"{prefix}_seconds_1gbit"] = ""
                else:
                    point = curve_point(curve, checkpoint)
                    row[f"{prefix}_wire_bytes"] = point["wire_bytes"]
                    row[f"{prefix}_decimal_mb"] = point["decimal_mb"]
                    row[f"{prefix}_seconds_1gbit"] = point["seconds_1gbit"]
            for build, (increment, endpoint) in enumerate(
                zip(build_wire, build_endpoints, strict=True), 1
            ):
                row[f"build{build}_incremental_wire_bytes"] = increment
                row[f"build{build}_cumulative_wire_bytes"] = endpoint["wire_bytes"]
            row["full_build_wire_bytes"] = first["wire_bytes"]
            row["full_build_decimal_mb"] = first["decimal_mb"]
            row["full_build_seconds_1gbit"] = first["seconds_1gbit"]
            row["full_build_ratio"] = first["ratio"]
            row["four_build_cumulative_wire_bytes"] = fourth["wire_bytes"]
            row["four_build_cumulative_decimal_mb"] = fourth["decimal_mb"]
            row["four_build_cumulative_seconds_1gbit"] = fourth["seconds_1gbit"]
            row["four_build_cumulative_ratio"] = fourth["ratio"]
            row["exact"] = True
            rows.append(row)
    return rows


def write_tsv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=tuple(rows[0]), delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def format_mb_time(wire_bytes: int) -> str:
    mb = wire_bytes / DECIMAL_MB
    mb_text = f"{mb:.0f}" if mb >= 10 else f"{mb:.1f}"
    seconds = wire_bytes / ONE_GBIT_BYTES_PER_SECOND
    if seconds >= 1:
        seconds_text = f"{seconds:.2f} s"
    elif seconds >= 0.1:
        seconds_text = f"{seconds:.2f} s"
    else:
        seconds_text = f"{seconds:.3f} s"
    return f"{mb_text} MB · {seconds_text}"


def format_s_transfer(shared_package: str, wire_bytes: int) -> str:
    if shared_package == "none" and wire_bytes == 0:
        return "none · 0 B"
    return format_mb_time(wire_bytes) + " once"


def aggregate_by_stage(rows: list[dict]) -> list[dict]:
    output = []
    for schema in COLD_SCHEMAS:
        selected = [row for row in rows if row["schema"] == schema["schema"]]
        raw = sum(int(row["raw_bytes_per_build"]) for row in selected)
        increments = [
            sum(int(row[f"build{build}_incremental_wire_bytes"]) for row in selected)
            for build in range(1, 5)
        ]
        cumulative = sum(increments)
        output.append(
            {
                "schema": schema["schema"],
                "raw_bytes_per_build": raw,
                "build_incremental_wire_bytes": increments,
                "four_build_cumulative_wire_bytes": cumulative,
                "first_build_ratio": raw / increments[0],
                "four_build_cumulative_ratio": (BUILD_REPETITIONS * raw) / cumulative,
            }
        )
    return output


def load_execution_provenance(run_root: Path, repository: Path) -> dict:
    specifications = (
        (
            "quietbox2",
            run_root / "run-quietbox2.json",
            {project for project, _ in CORPORA} - {"opencv", "leveldb"},
        ),
        ("nas642", run_root / "run.json", {"opencv", "leveldb"}),
    )
    profiles = []
    all_runs = set()
    expected_schemas = {schema["schema"] for schema in COLD_SCHEMAS}
    for host, path, expected_projects in specifications:
        metadata = json.loads(path.read_text())
        if int(metadata["build_repetitions"]) != BUILD_REPETITIONS:
            raise ValueError(f"{path}: build repetition count differs")
        if {schema["schema"] for schema in metadata["schemas"]} != expected_schemas:
            raise ValueError(f"{path}: cold schema set differs")
        run_keys = {(row["corpus"], row["schema"]) for row in metadata["runs"]}
        expected_keys = {
            (project, schema) for project in expected_projects for schema in expected_schemas
        }
        if run_keys != expected_keys:
            raise ValueError(f"{path}: project/schema run set differs")
        if all_runs & run_keys:
            raise ValueError(f"{path}: duplicate project/schema run")
        all_runs.update(run_keys)
        profiles.append(
            {
                "host": host,
                "metadata": str(path.resolve()),
                "metadata_sha256": sha256(path),
                "codec_binary": metadata["codec"],
                "codec_binary_sha256": metadata["codec_sha256"],
                "projects": sorted(expected_projects),
                "exact_runs": len(run_keys),
            }
        )
    if len(all_runs) != len(CORPORA) * len(COLD_SCHEMAS):
        raise ValueError(f"execution provenance covers {len(all_runs)} rows instead of 80")
    return {
        "codec_source_sha256": sha256(repository / "linecache/codec50.cpp"),
        "runner_source_sha256": sha256(
            repository / "linecache/run_cold_four_build_matrix.py"
        ),
        "profiles": profiles,
    }


def render_markdown(
    rows: list[dict],
    aggregates: list[dict],
    provenance: dict,
    checkpoints: tuple[int, ...],
    run_root: Path,
) -> str:
    lines = [
        "# Cold retained-state P25-P29 transfer matrix",
        "",
        "This supersedes the earlier 240-row presentation. The matrix contains exactly "
        "**80 cold rows: 16 projects × P25 through P29**. Bit-0 and bit-1 are separate "
        "half-cache acceptance evidence and are intentionally not repeated here.",
        "",
        "**Scope boundary:** every row here is a cold **no-shared-package** row. F starts "
        "with neither learned generation state nor an installed `S` bootstrap package. "
        "This is a single native/source-visible capture per project; it is not the "
        "25-project inventory and it is not the four-profile Docker matrix. In particular, "
        "GCC, Firefox, Qt6, ClickHouse, PyTorch, Folly, Arrow, Bitcoin, and V8 are absent.",
        "See the [corpus coverage ledger](CORPUS-COVERAGE-STATUS.md) for the complete "
        "native and four-profile pilot inventories.",
        "",
        "Each codec runs one manifest containing the same project build four times in the "
        "same order. F starts empty for build 1 and retains learned objects for builds 2--4. "
        "`full build` is the cumulative endpoint after build 1; `4× full build` is the "
        "cumulative endpoint after all four builds, not merely the fourth-build increment.",
        "",
        "Every TU reconstructs exactly. Every first-build endpoint is required to equal its "
        "previously retained complete P25, P26, P27, P28, or P29 cold ledger byte-for-byte. "
        "Thus repeating the manifest did not change any first-build decision.",
        "",
        "Cells show cumulative decimal MB and computed ideal payload time at 1 Gbit/s. "
        "The link rate is fixed at 125,000,000 bytes/s; these times are calculations, not "
        "codec-throughput measurements.",
        "",
        "## Schema legend",
        "",
        "| schema | complete-codec change |",
        "|---|---|",
    ]
    for schema in COLD_SCHEMAS:
        lines.append(f"| `{schema['schema']}` | {schema['description']} |")
    lines.extend(
        [
            "",
            "Here `S1 chain` names the causal superblock-sequence transform inside P28/P29. "
            "It is not an installed pre-shared `S` package; all rows below have no such package.",
            "",
            "## Matrix",
            "",
            "| project | schema | start | "
            + " | ".join(f"TU {checkpoint}" for checkpoint in checkpoints)
            + " | full build | S one-time transfer | 4× full build |",
            "|---|---|---|" + "---:|" * (len(checkpoints) + 3),
        ]
    )
    for row in rows:
        cells = []
        for checkpoint in checkpoints:
            value = row[f"tu{checkpoint}_wire_bytes"]
            cells.append("—" if value == "" else format_mb_time(int(value)))
        lines.append(
            f"| {row['project']} | `{row['schema']}` | cold / no S | "
            + " | ".join(cells)
            + f" | {format_mb_time(int(row['full_build_wire_bytes']))}"
            + f" | {format_s_transfer(row['shared_package'], int(row['s_one_time_transfer_bytes']))}"
            + f" | {format_mb_time(int(row['four_build_cumulative_wire_bytes']))} |"
        )
    lines.extend(
        [
            "",
            "## Aggregate retained-state learning",
            "",
            "The build columns below are incremental wire bytes for each pass. The final "
            "column is the ratio of four complete raw builds to their cumulative wire.",
            "",
            "| schema | build 1 | build 2 | build 3 | build 4 | cumulative 4× | "
            "build-1 ratio | cumulative ratio |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in aggregates:
        increments = row["build_incremental_wire_bytes"]
        lines.append(
            f"| `{row['schema']}` | "
            + " | ".join(format_mb_time(value) for value in increments)
            + f" | {format_mb_time(int(row['four_build_cumulative_wire_bytes']))}"
            + f" | {float(row['first_build_ratio']):.2f}×"
            + f" | {float(row['four_build_cumulative_ratio']):.2f}× |"
        )
    lines.extend(
        [
            "",
            "## Execution and accounting",
            "",
            "```text",
            "decimal_MB = cumulative_complete_protocol_wire_bytes / 1,000,000",
            "ideal_seconds_at_1_Gbit/s = cumulative_complete_protocol_wire_bytes / 125,000,000",
            "```",
            "",
            "The machine TSV retains exact byte counts, computed seconds, per-build incremental "
            "wire, cumulative endpoints, one-time `S` transfer, ratios, and log/curve hashes. "
            "For every row in this no-shared experiment, `S one-time transfer = 0 B`. A later "
            "row using an installed package must report that package once in this separate "
            "column; it must not add the package to every build.",
            "",
            "The source-visible execution profile uses 70 quietbox2 rows and 10 capture-host "
            "rows. OpenCV and LevelDB retain original absolute source paths, so all five cold "
            "schemas for each were rerun on the capture host. This keeps material decisions "
            "consistent across stages.",
            "",
            f"Codec source SHA-256: `{provenance['codec_source_sha256']}`. Runner source "
            f"SHA-256: `{provenance['runner_source_sha256']}`. Quietbox2 binary SHA-256: "
            f"`{provenance['profiles'][0]['codec_binary_sha256']}`. Capture-host binary "
            f"SHA-256: `{provenance['profiles'][1]['codec_binary_sha256']}`.",
            "",
            f"The 80 curves and 80 complete logs are retained under `{run_root.resolve()}`.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument("--checkpoints", nargs="+", type=int, default=DEFAULT_CHECKPOINTS)
    parser.add_argument("--tsv", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()
    checkpoints = tuple(args.checkpoints)
    if not checkpoints or any(value <= 0 for value in checkpoints):
        raise ValueError("positive checkpoints are required")
    rows = make_rows(args.run_root, args.repository, checkpoints)
    if len(rows) != len(CORPORA) * len(COLD_SCHEMAS):
        raise ValueError(f"expected 80 rows, found {len(rows)}")
    aggregates = aggregate_by_stage(rows)
    provenance = load_execution_provenance(args.run_root, args.repository)
    for path in (args.tsv, args.json, args.markdown):
        path.parent.mkdir(parents=True, exist_ok=True)
    write_tsv(args.tsv, rows)
    summary = {
        "experiment": "cold P25-P29 retained-state four-build TU matrix",
        "supersedes": "240-row cold/bit0/bit1 one-build presentation",
        "execution_profile": {
            "name": "source-visible consistent",
            "quietbox2_rows": 70,
            "capture_host_opencv_leveldb_rows": 10,
            "reason": (
                "OpenCV and LevelDB .ii marker paths resolve only on the capture host; "
                "all five cold schemas for those projects use that host"
            ),
        },
        "accounting": {
            "decimal_mb_divisor": DECIMAL_MB,
            "one_gbit_bytes_per_second": ONE_GBIT_BYTES_PER_SECOND,
            "transfer_times_are_computed": True,
            "s_transfer_is_one_time_and_separate_from_build_wire": True,
            "shared_package": "none",
            "s_one_time_transfer_bytes": NO_SHARED_S_TRANSFER_BYTES,
        },
        "build_repetitions": BUILD_REPETITIONS,
        "checkpoints": list(checkpoints),
        "retained_run_root": str(args.run_root.resolve()),
        "exact_rows": len(rows),
        "first_build_historical_matches": len(rows),
        "provenance": provenance,
        "aggregate_by_schema": aggregates,
        "rows": rows,
    }
    args.json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    args.markdown.write_text(
        render_markdown(rows, aggregates, provenance, checkpoints, args.run_root)
    )
    print(f"wrote {len(rows)} exact cold retained-state rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
