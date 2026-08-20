#!/usr/bin/env python3
"""Build an exact P29 physical-ledger JSONL for a one-route scenario.

The codec is run once to create its two typed directional streams and again in sink-replay
mode.  The resulting frame slices are converted into the Root/Need/Fill/fallback/close/Ack
dialogue consumed by ``run_scenario.py``.  Every per-TU directional sum is checked against
the codec's physical sink curve and the selector's C-to-F transaction total.

P29's current producer has one materialized route.  This builder therefore refuses F>1
instead of labelling independent per-route processes as the shared-C P29 design.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
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
    "--route-s1",
    "1",
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


def scenario_items(scenario: sim.LoadedScenario) -> list[sim.WorkItem]:
    if int(scenario.document["environments"]["env_count"]) != 1:
        raise ValueError("P29 physical builder currently requires exactly one C authority")
    if int(scenario.document["workers"]["f_count"]) != 1:
        raise ValueError("P29 physical builder refuses F>1 until shared-C multi-route is materialized")
    diagnostic = sim.Simulator(
        scenario, sim.CompileOnlyAdapter(), snapshot_interval_ns=10**30
    ).run()
    by_key = {
        item.key: item
        for items in scenario.work_items.values()
        for item in items
    }
    items = [
        by_key[(row["workload"], int(row["build"]), int(row["logical"]))]
        for row in diagnostic.assignments
    ]
    if any(int(row["worker"]) != 0 for row in diagnostic.assignments):
        raise AssertionError("one-F diagnostic assignment selected a different F")
    return items


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
    items = scenario_items(scenario)
    for item in items:
        if not item.payload.is_file():
            raise ValueError(f"payload is absent: {item.payload}")
        if item.payload.stat().st_size != item.raw_bytes:
            raise ValueError(f"payload size differs from trace for {item.key}")
    work.mkdir(parents=True, exist_ok=True)
    manifest = work / "manifest.txt"
    manifest.write_text("".join(f"{item.payload}\n" for item in items))
    c_sink, f_sink = work / "p29.c-to-f.bin", work / "p29.f-to-c.bin"
    curve, selector = work / "sink-curve.tsv", work / "selector.tsv"
    components = work / "components.tsv"
    common = [
        str(codec.resolve()),
        "--manifest",
        str(manifest),
        *P29_OPTIONS,
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
    encode_stdout, encode_stderr = work / "encode.stdout", work / "encode.stderr"
    checked_run(common, encode_stdout, encode_stderr)
    encode_text = encode_stdout.read_text(errors="replace")
    for marker in (
        "byte-exact=OK",
        "SELECTOR closure:",
        "SELECTOR manifest:",
        "SELECTOR full total:",
    ):
        if marker not in encode_text:
            raise RuntimeError(f"P29 encode output lacks required record: {marker}")
    replay_stdout, replay_stderr = work / "replay.stdout", work / "replay.stderr"
    checked_run([*common, "--sink-replay"], replay_stdout, replay_stderr)
    replay_text = replay_stdout.read_text(errors="replace")
    if "SINK REPLAY OK:" not in replay_text or "byte-exact=OK" not in replay_text:
        raise RuntimeError("P29 directional sink replay did not report complete exact replay")

    curve_rows = read_tsv(curve)
    selector_rows = read_tsv(selector)
    component_rows = read_tsv(components)
    if not (len(curve_rows) == len(selector_rows) == len(component_rows) == len(items)):
        raise RuntimeError("P29 per-TU output row counts differ from the scenario")
    c_data, f_data = c_sink.read_bytes(), f_sink.read_bytes()
    c_prior = f_prior = 0
    emitted_blocks = 0
    payload_digests: dict[Path, str] = {}
    ledger_rows: list[dict[str, object]] = []
    c_total = f_total = 0
    for route_sequence, (item, curve_row, selector_row, component_row) in enumerate(
        zip(items, curve_rows, selector_rows, component_rows)
    ):
        expected_tu = route_sequence + 1
        if int(curve_row["tu"]) != expected_tu or int(component_row["tu"]) != expected_tu:
            raise RuntimeError(f"P29 curve order differs at TU {expected_tu}")
        if int(selector_row["tu"]) != route_sequence:
            raise RuntimeError(f"P29 selector order differs at TU {expected_tu}")
        c_end, f_end = int(curve_row["cf_offset"]), int(curve_row["fc_offset"])
        c_frames = parse_frames(c_data, c_prior, c_end, f"C-to-F TU {expected_tu}")
        f_frames = parse_frames(f_data, f_prior, f_end, f"F-to-C TU {expected_tu}")
        phases = phase_rows(c_frames, f_frames)
        graph = transaction_graph(phases)
        c_delta, f_delta = c_end - c_prior, f_end - f_prior
        if int(selector_row["actual_delta"]) != c_delta:
            raise RuntimeError(f"selector C-to-F bytes differ at TU {expected_tu}")
        if component_row["exact"] != "true":
            raise RuntimeError(f"component reconstruction row is not exact at TU {expected_tu}")
        if int(curve_row["raw_bytes"]) != item.raw_bytes:
            raise RuntimeError(f"P29 raw bytes differ at TU {expected_tu}")
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
        ledger_rows.append(
            {
                "record": "tu",
                "workload": item.workload,
                "build": item.build,
                "logical": item.logical,
                "worker": 0,
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
                },
                "exact": True,
            }
        )
        c_total += c_delta
        f_total += f_delta
        c_prior, f_prior = c_end, f_end
    if c_prior != len(c_data) or f_prior != len(f_data):
        raise RuntimeError("P29 sink curve does not consume both directional streams")

    command_display = [str(codec.resolve()), "--manifest", str(manifest), *P29_OPTIONS, *extra_options]
    descriptor = {
        "record": "physical-ledger",
        "schema": "icecream-physical-codec-ledger-v1",
        "codec": "p29",
        "scenario": scenario.document["name"],
        "scenario_sha256": sim.sha256(scenario.path),
        "assignment": "common simulator compile-only dispatch order; one materialized route",
        "dialogue_window_per_route": 1,
        "command": command_display,
        "codec_binary": str(codec.resolve()),
        "codec_binary_sha256": sha256(codec.resolve()),
        "manifest_sha256": sha256(manifest),
        "directional_streams": {
            "c_to_f": {"bytes": len(c_data), "sha256": sha256(c_sink)},
            "f_to_c": {"bytes": len(f_data), "sha256": sha256(f_sink)},
        },
        "reconstruction": {
            "status": "pass",
            "method": "codec byte comparison plus typed directional sink replay",
            "encode_stdout_sha256": sha256(encode_stdout),
            "replay_stdout_sha256": sha256(replay_stdout),
        },
        "codec_cpu": "measured by the codec run, not scheduled in this ledger revision",
        "limitations": [
            "one C authority and one F route",
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
