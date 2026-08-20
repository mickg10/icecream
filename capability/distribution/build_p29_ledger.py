#!/usr/bin/env python3
"""Build an exact shared-C P29 physical-ledger JSONL for a multi-F scenario.

Every route run reads the same complete manifest and TU-to-F map, deterministically replays
one global catalogue plus every ordered route plan, and then materializes only its selected F
receiver.  The builder requires the global plan digest to match across runs before combining
their typed directional streams.  Resulting frame slices become the Root/Need/Fill/fallback/
close/Ack dialogues consumed by ``run_scenario.py``.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("run_scenario.py")
SPEC = importlib.util.spec_from_file_location("distribution_run_scenario", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {MODULE_PATH}")
sim = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sim
SPEC.loader.exec_module(sim)


P29_OPTIONS = (
    "--z",
    "3",
    "--mixed-regions",
    "--byte-array-lines",
    "--direct-ordinals",
    "--compressed-blobs",
    "--blob-threads",
    "8",
    "--blob-lazy-fallback",
    "--mo-factor",
    "--s1-max-chain",
    "1024",
    "--blob-z",
    "9",
    "--blob-zstd-workers",
    "4",
    "--blob-zstd-job-mib",
    "5",
    "--blob-zstd-overlap-log",
    "3",
    "--stable-root-tags",
    "--literal-ondemand",
    "--literal-group-skip-zstd10",
    "--transactional-tu",
    "--live-selector",
)

TYPE_NAMES = {
    1: "root",
    2: "block-definition",
    3: "need",
    4: "association",
    5: "path-definition",
    6: "line-definition",
    7: "region-definition",
    8: "fill-control",
    9: "fill-literal",
    10: "fill-array-control",
    11: "fill-array-values",
    12: "fill-source-control",
    13: "fill-source-files",
    20: "selector",
    21: "blob",
    22: "blob-patch",
    23: "literal-group",
    30: "fallback-request",
    31: "fallback-reply",
    0xFD: "ack",
    0xFE: "tu-close",
    0xFF: "build-close",
}


@dataclass(frozen=True)
class Frame:
    kind: int
    bytes: int


@dataclass(frozen=True)
class AssignedItem:
    item: sim.WorkItem
    worker: int
    tu_seq: int
    rel_seq: int


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def parse_frames(data: bytes, start: int, end: int, label: str) -> list[Frame]:
    if start < 0 or end < start or end > len(data):
        raise ValueError(f"{label}: invalid physical slice [{start},{end})")
    frames = []
    offset = start
    while offset < end:
        if end - offset < 5:
            raise ValueError(f"{label}: truncated frame header at {offset}")
        kind = data[offset]
        payload = struct.unpack_from("<I", data, offset + 1)[0]
        frame_bytes = 5 + payload
        if frame_bytes > end - offset:
            raise ValueError(f"{label}: truncated type-{kind} frame at {offset}")
        if kind not in TYPE_NAMES:
            raise ValueError(f"{label}: unknown type-{kind} frame at {offset}")
        frames.append(Frame(kind, frame_bytes))
        offset += frame_bytes
    if offset != end:
        raise AssertionError("frame parser did not finish on the requested boundary")
    return frames


def summed(frames: list[Frame], kinds: set[int]) -> int:
    return sum(frame.bytes for frame in frames if frame.kind in kinds)


def phase_rows(c_frames: list[Frame], f_frames: list[Frame]) -> list[dict[str, object]]:
    """Map typed frames onto their causal transaction order without changing a byte."""
    initial_types = {1, 2, 4}
    lines_types = {8, 9, 10, 11, 12, 13, 20, 21, 22, 23}
    definition_types = {5, 6, 7}
    close_types = {0xFE, 0xFF}
    c_fallback_types = {31}
    f_need_types = {3}
    f_fallback_types = {30}
    f_ack_types = {0xFD, 0xFE, 0xFF}
    classified_c = (
        initial_types
        | lines_types
        | definition_types
        | close_types
        | c_fallback_types
    )
    classified_f = f_need_types | f_fallback_types | f_ack_types
    phases: list[dict[str, object]] = []

    def add(name: str, direction: str, byte_count: int) -> None:
        if byte_count:
            phases.append({"name": name, "direction": direction, "bytes": byte_count})

    add("p29-root", "c_to_f", summed(c_frames, initial_types))
    add("p29-need", "f_to_c", summed(f_frames, f_need_types))
    add("p29-lines", "c_to_f", summed(c_frames, lines_types))
    add("p29-fill", "c_to_f", summed(c_frames, definition_types))
    add("p29-fallback-request", "f_to_c", summed(f_frames, f_fallback_types))
    add("p29-fallback-reply", "c_to_f", summed(c_frames, c_fallback_types))
    add("p29-close", "c_to_f", summed(c_frames, close_types))
    add("p29-ack", "f_to_c", summed(f_frames, f_ack_types))
    unknown_c = [frame.kind for frame in c_frames if frame.kind not in classified_c]
    if unknown_c:
        raise ValueError(f"unclassified C-to-F frame types: {unknown_c}")
    unknown_f = [frame.kind for frame in f_frames if frame.kind not in classified_f]
    if unknown_f:
        raise ValueError(f"unclassified F-to-C frame types: {unknown_f}")
    if sum(int(row["bytes"]) for row in phases if row["direction"] == "c_to_f") != sum(
        frame.bytes for frame in c_frames
    ):
        raise AssertionError("C-to-F phase split does not tile its physical frame slice")
    if sum(int(row["bytes"]) for row in phases if row["direction"] == "f_to_c") != sum(
        frame.bytes for frame in f_frames
    ):
        raise AssertionError("F-to-C phase split does not tile its physical frame slice")
    return phases


def transaction_graph(phases: list[dict[str, object]]) -> dict[str, object]:
    """Project exact P29 frames onto the Protocol-50 fork/join dialogue."""
    names = {str(phase["name"]) for phase in phases}
    dependencies: dict[str, list[str]] = {}
    priorities = {
        "p29-root": 3,
        "p29-need": 1,
        "p29-lines": 4,
        "p29-fill": 2,
        "p29-fallback-request": 1,
        "p29-fallback-reply": 2,
        "p29-close": 0,
        "p29-ack": 0,
    }
    if "p29-root" not in names or "p29-close" not in names:
        raise ValueError("P29 transaction lacks Root or close frames")
    dependencies["p29-root"] = []
    if "p29-need" in names:
        dependencies["p29-need"] = ["p29-root:delivered"]
    if "p29-lines" in names:
        dependencies["p29-lines"] = ["p29-root:sent"]
    if "p29-fill" in names:
        dependencies["p29-fill"] = [
            "p29-need:delivered" if "p29-need" in names else "p29-root:delivered"
        ]
    material = [
        f"{name}:delivered"
        for name in ("p29-lines", "p29-fill")
        if name in names
    ]
    if "p29-need" in names:
        material.append("p29-need:delivered")
    if "p29-fallback-request" in names:
        dependencies["p29-fallback-request"] = material or ["p29-root:delivered"]
    if "p29-fallback-reply" in names:
        if "p29-fallback-request" not in names:
            raise ValueError("P29 fallback reply has no request")
        dependencies["p29-fallback-reply"] = [
            "p29-fallback-request:delivered"
        ]
        material.append("p29-fallback-reply:delivered")
    dependencies["p29-close"] = material or ["p29-root:delivered"]
    if "p29-ack" in names:
        dependencies["p29-ack"] = ["p29-close:delivered"]
    graph_phases = []
    for phase in phases:
        name = str(phase["name"])
        graph_phases.append(
            {
                **phase,
                "priority": priorities[name],
                "depends_on": dependencies[name],
            }
        )
    return {
        "phases": graph_phases,
        "initial_tokens": ["attachment:accepted"],
        "input_ready_after": ["attachment:accepted", "p29-close:delivered"],
        "commit_after": [
            "p29-ack:delivered" if "p29-ack" in names else "p29-close:delivered"
        ],
    }


def checked_run(command: list[str], stdout_path: Path, stderr_path: Path) -> None:
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
    if result.returncode:
        tail = stderr_path.read_text(errors="replace")[-3000:]
        raise RuntimeError(
            f"codec exited {result.returncode}: {' '.join(command)}\n{tail}"
        )


def scenario_items(scenario: sim.LoadedScenario) -> list[AssignedItem]:
    if int(scenario.document["environments"]["env_count"]) != 1:
        raise ValueError("P29 physical builder currently requires exactly one C authority")
    diagnostic = sim.Simulator(
        scenario, sim.CompileOnlyAdapter(), snapshot_interval_ns=10**30
    ).run()
    by_key = {
        item.key: item
        for items in scenario.work_items.values()
        for item in items
    }
    assigned = []
    for expected_tu_seq, row in enumerate(diagnostic.assignments):
        item = by_key[(row["workload"], int(row["build"]), int(row["logical"]))]
        tu_seq = int(row["tu_seq"])
        if tu_seq != expected_tu_seq:
            raise AssertionError("diagnostic assignment is not in contiguous TU_SEQ order")
        assigned.append(
            AssignedItem(item, int(row["worker"]), tu_seq, int(row["rel_seq"]))
        )
    if len(assigned) != len(by_key):
        raise AssertionError("diagnostic assignment did not cover every scenario TU")
    return assigned


def shared_plan_record(output: str) -> dict[str, object]:
    matches = re.findall(
        r"^MULTIROUTE_PLAN routes=(\d+) target=(\d+) active_tus=(\d+) "
        r"blocks=(\d+) digest=([0-9a-f]{32})$",
        output,
        re.MULTILINE,
    )
    if len(matches) != 1:
        raise RuntimeError("P29 output does not contain exactly one multi-route plan record")
    routes, target, active_tus, blocks, digest = matches[0]
    return {
        "routes": int(routes),
        "target": int(target),
        "active_tus": int(active_tus),
        "blocks": int(blocks),
        "digest": digest,
    }


def build_ledger(
    scenario_path: Path,
    codec: Path,
    output_path: Path,
    work: Path,
    extra_options: list[str],
) -> None:
    scenario = sim.load_scenario(scenario_path)
    if not codec.is_file():
        raise ValueError(f"P29 codec binary is absent: {codec}")
    assigned = scenario_items(scenario)
    items = [entry.item for entry in assigned]
    for item in items:
        if not item.payload.is_file():
            raise ValueError(f"payload is absent: {item.payload}")
        if item.payload.stat().st_size != item.raw_bytes:
            raise ValueError(f"payload size differs from trace for {item.key}")
    work.mkdir(parents=True, exist_ok=True)
    manifest = work / "manifest.txt"
    manifest.write_text("".join(f"{item.payload}\n" for item in items))
    worker_count = int(scenario.document["workers"]["f_count"])
    route_map = work / "route-map.txt"
    route_map.write_text(
        f"p29-route-map-v1 {worker_count} {len(assigned)}\n"
        + "".join(f"{entry.worker}\n" for entry in assigned)
    )
    by_worker: dict[int, list[AssignedItem]] = defaultdict(list)
    for entry in assigned:
        by_worker[entry.worker].append(entry)
    for worker, route in by_worker.items():
        if [entry.rel_seq for entry in route] != list(range(len(route))):
            raise AssertionError(f"P29 F{worker} projection is not contiguous")

    shared_digest: str | None = None
    shared_blocks: int | None = None
    route_metadata: dict[str, object] = {}
    ledger_by_item: dict[tuple[str, int, int], dict[str, object]] = {}
    payload_digests: dict[Path, str] = {}
    c_total = f_total = 0
    required_markers = (
        "byte-exact=OK",
        "SELECTOR closure:",
        "SELECTOR manifest:",
        "SELECTOR full total:",
    )
    for worker, route in sorted(by_worker.items()):
        route_name = f"C0-F{worker}"
        route_directory = work / route_name
        route_directory.mkdir(parents=True, exist_ok=True)
        c_sink = route_directory / "p29.c-to-f.bin"
        f_sink = route_directory / "p29.f-to-c.bin"
        curve = route_directory / "sink-curve.tsv"
        selector = route_directory / "selector.tsv"
        components = route_directory / "components.tsv"
        common = [
            str(codec.resolve()),
            "--manifest",
            str(manifest),
            *P29_OPTIONS,
            "--route-s1",
            str(worker_count),
            "--route-map",
            str(route_map),
            "--materialize-route",
            str(worker),
            *extra_options,
            "--selector-tsv",
            str(selector),
            "--sink-curve",
            str(curve),
            "--component-curve-tsv",
            str(components),
            "--cf-sink",
            str(c_sink),
            "--fc-sink",
            str(f_sink),
        ]
        encode_stdout = route_directory / "encode.stdout"
        encode_stderr = route_directory / "encode.stderr"
        checked_run(common, encode_stdout, encode_stderr)
        encode_text = encode_stdout.read_text(errors="replace")
        for marker in required_markers:
            if marker not in encode_text:
                raise RuntimeError(f"P29 F{worker} encode output lacks: {marker}")
        plan = shared_plan_record(encode_text)
        if (
            plan["routes"] != worker_count
            or plan["target"] != worker
            or plan["active_tus"] != len(route)
        ):
            raise RuntimeError(f"P29 F{worker} plan record differs from its projection")
        if shared_digest is None:
            shared_digest = str(plan["digest"])
            shared_blocks = int(plan["blocks"])
        elif plan["digest"] != shared_digest or plan["blocks"] != shared_blocks:
            raise RuntimeError("P29 route runs produced different shared-C plans")

        replay_stdout = route_directory / "replay.stdout"
        replay_stderr = route_directory / "replay.stderr"
        checked_run([*common, "--sink-replay"], replay_stdout, replay_stderr)
        replay_text = replay_stdout.read_text(errors="replace")
        if "SINK REPLAY OK:" not in replay_text or "byte-exact=OK" not in replay_text:
            raise RuntimeError(f"P29 F{worker} directional replay was not exact")
        if shared_plan_record(replay_text) != plan:
            raise RuntimeError(f"P29 F{worker} replay changed the shared plan record")

        curve_rows = [row for row in read_tsv(curve) if row["active"] == "1"]
        selector_rows = read_tsv(selector)
        component_rows = [
            row for row in read_tsv(components) if row["active"] == "1"
        ]
        if not (
            len(curve_rows)
            == len(selector_rows)
            == len(component_rows)
            == len(route)
        ):
            raise RuntimeError(f"P29 F{worker} per-TU row counts differ")
        c_data, f_data = c_sink.read_bytes(), f_sink.read_bytes()
        c_prior = f_prior = emitted_blocks = 0
        for route_sequence, (
            entry,
            curve_row,
            selector_row,
            component_row,
        ) in enumerate(zip(route, curve_rows, selector_rows, component_rows)):
            item = entry.item
            expected_tu = entry.tu_seq + 1
            if (
                int(curve_row["tu"]) != expected_tu
                or int(component_row["tu"]) != expected_tu
                or int(selector_row["tu"]) != entry.tu_seq
                or int(curve_row["rel_seq"]) != route_sequence
                or int(component_row["rel_seq"]) != route_sequence
                or int(selector_row["rel_seq"]) != route_sequence
            ):
                raise RuntimeError(f"P29 F{worker} order differs at TU_SEQ {entry.tu_seq}")
            if entry.rel_seq != route_sequence:
                raise AssertionError("P29 route order differs from simulator REL_SEQ")
            c_end, f_end = int(curve_row["cf_offset"]), int(curve_row["fc_offset"])
            c_frames = parse_frames(
                c_data, c_prior, c_end, f"C-to-F F{worker} TU {expected_tu}"
            )
            f_frames = parse_frames(
                f_data, f_prior, f_end, f"F-to-C F{worker} TU {expected_tu}"
            )
            phases = phase_rows(c_frames, f_frames)
            graph = transaction_graph(phases)
            c_delta, f_delta = c_end - c_prior, f_end - f_prior
            if int(selector_row["actual_delta"]) != c_delta:
                raise RuntimeError(f"P29 selector bytes differ at TU_SEQ {entry.tu_seq}")
            if component_row["exact"] != "true":
                raise RuntimeError(f"P29 reconstruction is not exact at TU_SEQ {entry.tu_seq}")
            if int(curve_row["raw_bytes"]) != item.raw_bytes:
                raise RuntimeError(f"P29 raw bytes differ at TU_SEQ {entry.tu_seq}")
            emitted_blocks += int(selector_row["emitted_blockdefs"])
            frame_types = defaultdict(int)
            for frame in c_frames:
                frame_types[f"c_to_f:{TYPE_NAMES[frame.kind]}"] += frame.bytes
            for frame in f_frames:
                frame_types[f"f_to_c:{TYPE_NAMES[frame.kind]}"] += frame.bytes
            payload = item.payload.resolve()
            raw_digest = payload_digests.get(payload)
            if raw_digest is None:
                raw_digest = sha256(payload)
                payload_digests[payload] = raw_digest
            ledger_by_item[item.key] = {
                "record": "tu",
                "workload": item.workload,
                "build": item.build,
                "logical": item.logical,
                "worker": worker,
                "tu_seq": entry.tu_seq,
                "rel_seq": route_sequence,
                "route_sequence": route_sequence,
                "raw_bytes": item.raw_bytes,
                "raw_sha256": raw_digest,
                "phases": graph["phases"],
                "initial_tokens": graph["initial_tokens"],
                "input_ready_after": graph["input_ready_after"],
                "commit_after": graph["commit_after"],
                "frame_bytes": dict(sorted(frame_types.items())),
                "state_after": {
                    "route_commits": route_sequence + 1,
                    "cumulative_c_to_f_bytes": c_end,
                    "cumulative_f_to_c_bytes": f_end,
                    "cumulative_emitted_block_definitions": emitted_blocks,
                    "selected_representation": selector_row["winner"],
                    "shared_plan_digest": shared_digest,
                },
                "exact": True,
            }
            c_total += c_delta
            f_total += f_delta
            c_prior, f_prior = c_end, f_end
        if c_prior != len(c_data) or f_prior != len(f_data):
            raise RuntimeError(f"P29 F{worker} curves do not consume both streams")
        route_metadata[route_name] = {
            "tus": len(route),
            "c_to_f_bytes": len(c_data),
            "f_to_c_bytes": len(f_data),
            "c_to_f_sha256": sha256(c_sink),
            "f_to_c_sha256": sha256(f_sink),
            "encode_stdout_sha256": sha256(encode_stdout),
            "replay_stdout_sha256": sha256(replay_stdout),
            "shared_plan_digest": shared_digest,
            "reconstruction": "pass",
            "typed_sink_replay": "pass",
        }

    ledger_rows = [ledger_by_item[entry.item.key] for entry in assigned]
    command_display = [
        str(codec.resolve()),
        "--manifest",
        str(manifest),
        *P29_OPTIONS,
        "--route-s1",
        str(worker_count),
        "--route-map",
        str(route_map),
        "--materialize-route",
        "F",
        *extra_options,
    ]
    descriptor = {
        "record": "physical-ledger",
        "schema": "icecream-physical-codec-ledger-v1",
        "codec": "p29",
        "scenario": scenario.document["name"],
        "scenario_sha256": sim.sha256(scenario.path),
        "assignment": "common simulator compile-only dispatch order; ordered per-F projections",
        "dialogue_window_per_route": 1,
        "command": command_display,
        "codec_binary": str(codec.resolve()),
        "codec_binary_sha256": sha256(codec.resolve()),
        "manifest_sha256": sha256(manifest),
        "route_map_sha256": sha256(route_map),
        "shared_plan_digest": shared_digest,
        "shared_block_count": shared_blocks,
        "directional_streams": {
            "c_to_f": {"bytes": c_total},
            "f_to_c": {"bytes": f_total},
        },
        "routes": route_metadata,
        "reconstruction": {
            "status": "pass",
            "method": "per-route codec byte comparison plus typed directional sink replay; shared plan digests equal",
        },
        "codec_cpu": "measured by route materializer runs, not scheduled in this ledger revision",
        "limitations": [
            "one C authority",
            "the deterministic shared catalogue/plan preparation is replayed per output route and digest-checked; its bytes and state are counted once logically",
            "route dialogue window fixed at one committed transaction",
        ],
    }
    final = {
        "record": "physical-summary",
        "totals": {
            "tus": len(ledger_rows),
            "c_to_f_bytes": c_total,
            "f_to_c_bytes": f_total,
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
    parser.add_argument("--codec-option", action="append", default=[])
    args = parser.parse_args()
    build_ledger(args.scenario, args.codec, args.out, args.work, args.codec_option)
    print(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
