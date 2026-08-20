#!/usr/bin/env python3
"""Build a multi-(C,F)-route GRZ physical ledger for the common simulator.

Each selected route is one persistent GRZ G2 stream containing exactly that route's
scheduler-selected TU subsequence.  ``--gtu 1`` closes one independently decodable frame per
TU.  The builder checks one group per TU, full byte reconstruction, deterministic retry, and
selected prefix cuts before emitting any physical-ledger row.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("run_scenario.py")
SPEC = importlib.util.spec_from_file_location("distribution_run_scenario", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {MODULE_PATH}")
sim = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sim
SPEC.loader.exec_module(sim)


GRZ_OPTIONS = (
    "-m",
    "g2",
    "-K",
    "256",
    "-s",
    "6",
    "-t",
    "21",
    "-l",
    "4",
    "-k",
    "5",
    "-b",
    "8",
    "-j",
    "8",
    "--gtu",
    "1",
    "--graw",
    "512",
    "--gadd",
    "128",
    "--hist",
    "1024",
)
END_FRAME_BYTES = 36


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_run(command: list[str], stdout_path: Path, stderr_path: Path) -> None:
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
    if result.returncode:
        tail = stderr_path.read_text(errors="replace")[-3000:]
        raise RuntimeError(
            f"GRZ command exited {result.returncode}: {' '.join(command)}\n{tail}"
        )


def read_curve(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def copy_prefix(source_path: Path, output_path: Path, byte_count: int) -> None:
    remaining = byte_count
    with source_path.open("rb") as source, output_path.open("wb") as output:
        while remaining:
            block = source.read(min(1 << 22, remaining))
            if not block:
                raise RuntimeError("source ended before requested prefix")
            output.write(block)
            remaining -= len(block)


def file_equals_prefix(actual_path: Path, expected_path: Path, byte_count: int) -> bool:
    if actual_path.stat().st_size != byte_count:
        return False
    remaining = byte_count
    with actual_path.open("rb") as actual, expected_path.open("rb") as expected:
        while remaining:
            count = min(1 << 22, remaining)
            left, right = actual.read(count), expected.read(count)
            if left != right or not left:
                return False
            remaining -= len(left)
    return True


def assignment_routes(
    scenario: sim.LoadedScenario,
) -> dict[tuple[int, int], list[sim.WorkItem]]:
    diagnostic = sim.Simulator(
        scenario, sim.CompileOnlyAdapter(), snapshot_interval_ns=10**30
    ).run()
    by_key = {
        item.key: item
        for items in scenario.work_items.values()
        for item in items
    }
    routes: dict[tuple[int, int], list[sim.WorkItem]] = defaultdict(list)
    for row in diagnostic.assignments:
        item = by_key[(row["workload"], int(row["build"]), int(row["logical"]))]
        routes[(item.environment, int(row["worker"]))].append(item)
    if sum(map(len, routes.values())) != len(by_key):
        raise AssertionError("diagnostic assignment did not cover every scenario TU")
    return dict(routes)


def write_route_input(
    route_directory: Path, items: list[sim.WorkItem]
) -> tuple[Path, Path]:
    manifest = route_directory / "manifest.txt"
    blob = route_directory / "input.ii"
    manifest.write_text("".join(f"{item.payload}\n" for item in items))
    with blob.open("wb") as output:
        for item in items:
            with item.payload.open("rb") as source:
                shutil.copyfileobj(source, output, 1 << 22)
    return manifest, blob


def prefix_points(count: int, stride: int, items: list[sim.WorkItem]) -> list[int]:
    points = {1, count}
    points.update(range(stride, count, stride))
    for index in range(count - 1):
        if items[index].build != items[index + 1].build:
            points.add(index + 1)
    if count <= 128:
        points.update(range(1, count + 1))
    return sorted(points)


def build_route(
    binary: Path,
    route_directory: Path,
    items: list[sim.WorkItem],
    extra_options: list[str],
    stride: int,
) -> tuple[list[int], dict[str, object]]:
    route_directory.mkdir(parents=True, exist_ok=True)
    manifest, blob = write_route_input(route_directory, items)
    tu_map = route_directory / "tu.map"
    checked_run(
        [str(binary), "tu", str(manifest), str(tu_map)],
        route_directory / "tu.stdout",
        route_directory / "tu.stderr",
    )
    container = route_directory / "route.grz"
    curve_path = route_directory / "curve.tsv"
    encode_command = [
        str(binary),
        "enc",
        str(blob),
        str(container),
        "-u",
        str(tu_map),
        *GRZ_OPTIONS,
        *extra_options,
        "--curve",
        str(curve_path),
    ]
    checked_run(
        encode_command,
        route_directory / "encode.stdout",
        route_directory / "encode.stderr",
    )
    curve = read_curve(curve_path)
    if len(curve) != len(items):
        raise RuntimeError("GRZ curve does not contain exactly one row per route TU")
    group_bytes = []
    for expected, row in enumerate(curve):
        if (
            int(row["group"]) != expected
            or int(row["tu_lo"]) != expected
            or int(row["tu_hi"]) != expected + 1
            or row["closed_by"] != "tu"
        ):
            raise RuntimeError(f"GRZ row {expected} is not one complete current-TU frame")
        if int(row["out_bytes"]) != items[expected].raw_bytes:
            raise RuntimeError(f"GRZ row {expected} raw extent differs from the scenario")
        group_bytes.append(int(row["comp_bytes"]))
    header_bytes = container.stat().st_size - sum(group_bytes) - END_FRAME_BYTES
    if header_bytes <= 0:
        raise RuntimeError("GRZ stream header extent is not positive")

    decoded = route_directory / "decoded.ii"
    checked_run(
        [str(binary), "dec", str(container), str(decoded), "-j", "1"],
        route_directory / "decode.stdout",
        route_directory / "decode.stderr",
    )
    if sha256(decoded) != sha256(blob):
        raise RuntimeError("GRZ full decode differs from its route input")

    retry = route_directory / "retry.grz"
    retry_curve = route_directory / "retry-curve.tsv"
    retry_command = [
        str(binary),
        "enc",
        str(blob),
        str(retry),
        "-u",
        str(tu_map),
        *GRZ_OPTIONS,
        *extra_options,
        "--retry-test",
        "1",
        "--curve",
        str(retry_curve),
    ]
    checked_run(
        retry_command,
        route_directory / "retry.stdout",
        route_directory / "retry.stderr",
    )
    if sha256(retry) != sha256(container) or retry_curve.read_bytes() != curve_path.read_bytes():
        raise RuntimeError("GRZ retry changed the physical container or frame curve")

    raw_offsets = [0]
    for item in items:
        raw_offsets.append(raw_offsets[-1] + item.raw_bytes)
    compressed_offset = header_bytes
    compressed_offsets = [header_bytes]
    for byte_count in group_bytes:
        compressed_offset += byte_count
        compressed_offsets.append(compressed_offset)
    checked_points = prefix_points(len(items), stride, items)
    for point in checked_points:
        prefix_container = route_directory / "prefix.grz"
        prefix_output = route_directory / "prefix.ii"
        copy_prefix(container, prefix_container, compressed_offsets[point])
        checked_run(
            [
                str(binary),
                "decprefix",
                str(prefix_container),
                str(prefix_output),
                "-g",
                str(point),
                "-j",
                "1",
            ],
            route_directory / "prefix.stdout",
            route_directory / "prefix.stderr",
        )
        if not file_equals_prefix(prefix_output, blob, raw_offsets[point]):
            raise RuntimeError(f"GRZ prefix through route TU {point} differs")

    physical_bytes = list(group_bytes)
    physical_bytes[0] += header_bytes
    physical_bytes[-1] += END_FRAME_BYTES
    if sum(physical_bytes) != container.stat().st_size:
        raise AssertionError("GRZ per-TU bytes do not tile the route container")
    metadata = {
        "tus": len(items),
        "raw_bytes": blob.stat().st_size,
        "wire_bytes": container.stat().st_size,
        "header_bytes": header_bytes,
        "end_frame_bytes": END_FRAME_BYTES,
        "container_sha256": sha256(container),
        "input_sha256": sha256(blob),
        "curve_sha256": sha256(curve_path),
        "prefix_points": checked_points,
        "full_reconstruction": "pass",
        "retry_identical": True,
    }
    decoded.unlink()
    retry.unlink()
    (route_directory / "prefix.grz").unlink(missing_ok=True)
    (route_directory / "prefix.ii").unlink(missing_ok=True)
    blob.unlink()
    return physical_bytes, metadata


def build_ledger(
    scenario_path: Path,
    binary: Path,
    output_path: Path,
    work: Path,
    extra_options: list[str],
    stride: int,
) -> None:
    scenario = sim.load_scenario(scenario_path)
    if not binary.is_file():
        raise ValueError(f"GRZ binary is absent: {binary}")
    routes = assignment_routes(scenario)
    for items in routes.values():
        for item in items:
            if not item.payload.is_file():
                raise ValueError(f"payload is absent: {item.payload}")
            if item.payload.stat().st_size != item.raw_bytes:
                raise ValueError(f"payload size differs from trace for {item.key}")
    work.mkdir(parents=True, exist_ok=True)
    ledger_by_item: dict[tuple[str, int, int], dict[str, object]] = {}
    route_metadata: dict[str, object] = {}
    payload_digests: dict[Path, str] = {}
    total_c_to_f = 0
    for (environment, worker), items in sorted(routes.items()):
        route_name = f"C{environment}-F{worker}"
        physical_bytes, metadata = build_route(
            binary.resolve(), work / route_name, items, extra_options, stride
        )
        route_metadata[route_name] = metadata
        cumulative = 0
        cumulative_raw = 0
        for route_sequence, (item, byte_count) in enumerate(zip(items, physical_bytes)):
            payload = item.payload.resolve()
            raw_digest = payload_digests.get(payload)
            if raw_digest is None:
                raw_digest = sha256(payload)
                payload_digests[payload] = raw_digest
            cumulative += byte_count
            cumulative_raw += item.raw_bytes
            total_c_to_f += byte_count
            ledger_by_item[item.key] = {
                "record": "tu",
                "workload": item.workload,
                "build": item.build,
                "logical": item.logical,
                "worker": worker,
                "route_sequence": route_sequence,
                "raw_bytes": item.raw_bytes,
                "raw_sha256": raw_digest,
                "phases": [
                    {
                        "name": "grz-current-tu-frame",
                        "direction": "c_to_f",
                        "bytes": byte_count,
                    }
                ],
                "state_after": {
                    "route_commits": route_sequence + 1,
                    "cumulative_c_to_f_bytes": cumulative,
                    "retained_history_bytes": min(
                        cumulative_raw, 1024 * 1024 * 1024
                    ),
                },
                "exact": True,
            }
    diagnostic = sim.Simulator(
        scenario, sim.CompileOnlyAdapter(), snapshot_interval_ns=10**30
    ).run()
    ledger_rows = [
        ledger_by_item[(row["workload"], int(row["build"]), int(row["logical"]))]
        for row in diagnostic.assignments
    ]
    descriptor = {
        "record": "physical-ledger",
        "schema": "icecream-physical-codec-ledger-v1",
        "codec": "grz",
        "scenario": scenario.document["name"],
        "scenario_sha256": sim.sha256(scenario.path),
        "assignment": "common simulator compile-only dispatch order, partitioned by (C,F)",
        "dialogue_window_per_route": 1,
        "command_options": list(GRZ_OPTIONS) + extra_options,
        "codec_binary": str(binary.resolve()),
        "codec_binary_sha256": sha256(binary.resolve()),
        "routes": route_metadata,
        "reconstruction": {
            "status": "pass",
            "method": "full route decode, deterministic retry, and exact selected prefix cuts",
        },
        "codec_cpu": "measured by route codec runs, not scheduled in this ledger revision",
        "limitations": [
            "one closed GRZ frame per scheduled TU",
            "route dialogue window fixed at one committed transaction",
        ],
    }
    final = {
        "record": "physical-summary",
        "totals": {
            "tus": len(ledger_rows),
            "c_to_f_bytes": total_c_to_f,
            "f_to_c_bytes": 0,
        },
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w") as output:
        for row in (descriptor, *ledger_rows, final):
            output.write(json.dumps(row, separators=(",", ":"), sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", type=Path)
    parser.add_argument("--codec", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--prefix-stride", type=int, default=64)
    parser.add_argument("--codec-option", action="append", default=[])
    args = parser.parse_args()
    if args.prefix_stride <= 0:
        raise ValueError("--prefix-stride must be positive")
    build_ledger(
        args.scenario,
        args.codec,
        args.out,
        args.work,
        args.codec_option,
        args.prefix_stride,
    )
    print(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
