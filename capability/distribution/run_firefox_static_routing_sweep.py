#!/usr/bin/env python3
"""Run and reconcile the exact Firefox static-routing width sweep.

The sweep keeps twenty available Fs in every cell and changes only the source-placement
policy: the existing round-robin control, then stable rendezvous frontiers of width
1, 2, 3, 4, 8, and 20.  Every policy is rebuilt separately with the real P29 and GRZ
codec, then replayed by the common simulator.  No producer timing is introduced.

The command is intentionally resumable at completed-cell granularity.  ``run`` refuses
to reuse an incomplete cell directory because a partial physical-codec work tree is not
evidence.  ``replay`` is narrower: it requires an already-complete, scenario-bound physical
ledger and refuses to overwrite any simulator output.  This permits a simulator-only
correction to reuse expensive codec evidence without silently rebuilding or mutating it.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import platform
import socket
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Iterable


HERE = Path(__file__).resolve().parent
BASE_SCENARIO = HERE / "scenarios" / "firefox-c1f20-200b1g.json"
TRACE = HERE / "firefox-corrected.compile-trace.tsv"
POLICIES: tuple[tuple[str, int | None], ...] = (
    ("round-robin", None),
    ("k1", 1),
    ("k2", 2),
    ("k3", 3),
    ("k4", 4),
    ("k8", 8),
    ("k20", 20),
)
CODECS = ("p29", "grz")
EXPECTED_TUS_PER_BUILD = 2_498
EXPECTED_BUILDS = 5
EXPECTED_JOBS = EXPECTED_TUS_PER_BUILD * EXPECTED_BUILDS


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def write_once(path: Path, payload: bytes) -> None:
    if path.exists():
        if path.read_bytes() != payload:
            raise RuntimeError(
                f"retained file differs from requested experiment: {path}"
            )
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def policy_width(policy: str) -> int | None:
    values = dict(POLICIES)
    if policy not in values:
        raise ValueError(f"unknown policy {policy!r}")
    return values[policy]


def scenario_document(policy: str, corpus_root: Path) -> dict[str, object]:
    width = policy_width(policy)
    document = json.loads(BASE_SCENARIO.read_text())
    document["name"] = f"firefox-corrected-C1F20_200B1G-static-sweep-{policy}"
    job = document["environments"]["job_selection"]["jobs"][0]
    job["trace"] = str(TRACE.resolve())
    job["corpus_root"] = str(corpus_root.resolve())
    job["builds"] = EXPECTED_BUILDS
    job["start_ns"] = 0
    job["build_release"] = {"mode": "after-previous", "gap_ns": 0}
    job["tu_release"] = {"mode": "all-at-zero"}
    if width is None:
        document["scheduler"] = {
            "ready_job_policy": "fifo-release",
            "placement_policy": "round-robin",
        }
    else:
        document["scheduler"] = {
            "ready_job_policy": "fifo-release",
            "placement_policy": "rendezvous",
            "dense_frontier_workers": width,
        }
    document["experiment"] = {
        "codecs": ["p29", "grz"],
        "routing_mode": "replay",
    }
    return document


def scenario_path(output: Path, policy: str) -> Path:
    return output / "scenarios" / f"firefox-{policy}.json"


def read_trace() -> tuple[int, int]:
    rows = 0
    raw_bytes = 0
    with TRACE.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        for row in reader:
            rows += 1
            raw_bytes += int(row["raw_bytes"])
    return rows, raw_bytes


def prepare(output: Path, corpus_root: Path, binaries: dict[str, Path]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    rows, raw_bytes = read_trace()
    if rows != EXPECTED_TUS_PER_BUILD:
        raise RuntimeError(f"corrected Firefox trace has {rows}, not 2,498 jobs")
    missing = []
    with TRACE.open(newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            payload = corpus_root / row["ii_relative"]
            if not payload.is_file() or payload.stat().st_size != int(row["raw_bytes"]):
                missing.append(str(payload))
                if len(missing) == 3:
                    break
    if missing:
        raise RuntimeError(f"Firefox payload check failed; first paths: {missing}")
    for codec, binary in binaries.items():
        if not binary.is_file():
            raise RuntimeError(f"{codec} binary is absent: {binary}")
    for policy, _ in POLICIES:
        write_once(
            scenario_path(output, policy),
            canonical_bytes(scenario_document(policy, corpus_root)),
        )
    manifest = {
        "schema": "icecream-firefox-static-routing-sweep-v1",
        "created_unix_ns": time.time_ns(),
        "host": socket.gethostname(),
        "platform": platform.platform(),
        "python": sys.version,
        "source_head": subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=HERE,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip(),
        "simulator_sha256": sha256(HERE / "run_scenario.py"),
        "p29_builder_sha256": sha256(HERE / "build_p29_ledger.py"),
        "grz_builder_sha256": sha256(HERE / "build_grz_ledger.py"),
        "base_scenario": str(BASE_SCENARIO),
        "base_scenario_sha256": sha256(BASE_SCENARIO),
        "trace": str(TRACE.resolve()),
        "trace_sha256": sha256(TRACE),
        "tus_per_build": rows,
        "builds": EXPECTED_BUILDS,
        "jobs_per_cell": rows * EXPECTED_BUILDS,
        "raw_bytes_per_build": raw_bytes,
        "raw_bytes_per_cell": raw_bytes * EXPECTED_BUILDS,
        "corpus_root": str(corpus_root.resolve()),
        "producer_timing": "not modeled; all TUs are released at generation start",
        "environment": "resident",
        "policies": [
            {"name": name, "dense_frontier_workers": width} for name, width in POLICIES
        ],
        "binaries": {
            codec: {
                "path": str(binary.resolve()),
                "sha256": sha256(binary.resolve()),
            }
            for codec, binary in binaries.items()
        },
    }
    manifest_path = output / "experiment-manifest.json"
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text())
        # Creation time is descriptive rather than part of experiment identity.
        manifest["created_unix_ns"] = previous["created_unix_ns"]
    write_once(manifest_path, canonical_bytes(manifest))


def command_record(command: list[str], cwd: Path) -> dict[str, object]:
    return {"argv": command, "cwd": str(cwd), "started_unix_ns": time.time_ns()}


def checked_timed_run(command: list[str], prefix: Path) -> None:
    record = command_record(command, HERE)
    write_once(prefix.with_suffix(".command.json"), canonical_bytes(record))
    timed = [
        "/usr/bin/time",
        "-v",
        "-o",
        str(prefix.with_suffix(".time")),
        *command,
    ]
    with (
        prefix.with_suffix(".stdout").open("w") as stdout,
        prefix.with_suffix(".stderr").open("w") as stderr,
    ):
        result = subprocess.run(timed, cwd=HERE, stdout=stdout, stderr=stderr)
    if result.returncode:
        tail = prefix.with_suffix(".stderr").read_text(errors="replace")[-4_000:]
        raise RuntimeError(
            f"command exited {result.returncode}: {' '.join(command)}\n{tail}"
        )


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def verify_ledger(
    ledger: Path,
    scenario: Path,
    codec: str,
    expected_binary: Path,
) -> dict[str, object]:
    descriptor: dict[str, object] | None = None
    final: dict[str, object] | None = None
    jobs = 0
    direction_bytes = defaultdict(int)
    phase_bytes = defaultdict(int)
    build_bytes: dict[int, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    assignments: dict[tuple[int, int], tuple[int, int, int]] = {}
    route_sequences: dict[int, list[int]] = defaultdict(list)
    tu_sequences: list[int] = []
    with ledger.open() as source:
        for line_number, line in enumerate(source, 1):
            row = json.loads(line)
            record = row["record"]
            if record == "physical-ledger":
                if descriptor is not None or line_number != 1:
                    raise RuntimeError(f"{ledger}: misplaced descriptor")
                descriptor = row
            elif record == "tu":
                jobs += 1
                build = int(row["build"])
                logical = int(row["logical"])
                worker = int(row["worker"])
                tu_seq = int(row["tu_seq"])
                rel_seq = int(row["rel_seq"])
                key = (build, logical)
                if key in assignments or not row["exact"]:
                    raise RuntimeError(f"{ledger}: repeated or inexact TU {key}")
                assignments[key] = (worker, tu_seq, rel_seq)
                route_sequences[worker].append(rel_seq)
                tu_sequences.append(tu_seq)
                for phase in row["phases"]:
                    direction = phase["direction"]
                    byte_count = int(phase["bytes"])
                    direction_bytes[direction] += byte_count
                    phase_bytes[(direction, phase["name"])] += byte_count
                    build_bytes[build][direction] += byte_count
            elif record == "physical-summary":
                final = row
            else:
                raise RuntimeError(f"{ledger}:{line_number}: unknown record {record!r}")
    if descriptor is None or final is None or jobs != EXPECTED_JOBS:
        raise RuntimeError(
            f"{ledger}: descriptor/final/job closure failed ({jobs} jobs)"
        )
    if descriptor["codec"] != codec:
        raise RuntimeError(f"{ledger}: codec differs")
    if descriptor["scenario_sha256"] != sha256(scenario):
        raise RuntimeError(f"{ledger}: scenario digest differs")
    if descriptor["codec_binary_sha256"] != sha256(expected_binary):
        raise RuntimeError(f"{ledger}: codec binary digest differs")
    if descriptor["reconstruction"]["status"] != "pass":
        raise RuntimeError(f"{ledger}: reconstruction is not pass")
    expected_totals = {
        "tus": jobs,
        "c_to_f_bytes": direction_bytes["c_to_f"],
        "f_to_c_bytes": direction_bytes["f_to_c"],
    }
    if final["totals"] != expected_totals:
        raise RuntimeError(f"{ledger}: final directional totals do not close")
    if sorted(tu_sequences) != list(range(jobs)):
        raise RuntimeError(f"{ledger}: TU_SEQ identity set is not contiguous")
    for worker, sequence in route_sequences.items():
        if sequence != list(range(len(sequence))):
            raise RuntimeError(f"{ledger}: F{worker} REL_SEQ is not contiguous")
    return {
        "descriptor": descriptor,
        "final": final,
        "assignments": assignments,
        "populated_workers": sorted(route_sequences),
        "phase_bytes": {
            f"{direction}:{phase}": byte_count
            for (direction, phase), byte_count in sorted(phase_bytes.items())
        },
        "build_bytes": {
            str(build): {
                "c_to_f_bytes": values["c_to_f"],
                "f_to_c_bytes": values["f_to_c"],
            }
            for build, values in sorted(build_bytes.items())
        },
    }


def verify_jsonl(
    path: Path, summary: dict[str, object], static: bool
) -> dict[str, int]:
    descriptor: dict[str, object] | None = None
    final: dict[str, object] | None = None
    interval_count = 0
    snapshot_count = 0
    active_ns = 0
    last_wall_end: int | None = None
    last_active_end = 0
    events = 0
    next_event_sequence = 0
    route_bound = 0
    flow_bytes = defaultdict(int)
    with path.open() as source:
        for line_number, line in enumerate(source, 1):
            row = json.loads(line)
            record = row["record"]
            if line_number == 1:
                if record != "experiment":
                    raise RuntimeError(f"{path}: descriptor is not first")
                descriptor = row
                continue
            if record == "summary":
                final = row
                continue
            if record not in {"snapshot", "gap"}:
                raise RuntimeError(f"{path}: unexpected timeline record {record!r}")
            interval_count += 1
            wall_start = int(row["wall_start_ns"])
            wall_end = int(row["wall_end_ns"])
            if last_wall_end is not None and wall_start != last_wall_end:
                raise RuntimeError(f"{path}: wall intervals do not tile")
            if int(row["wall_duration_ns"]) != wall_end - wall_start:
                raise RuntimeError(f"{path}: wall interval duration differs")
            last_wall_end = wall_end
            if record == "snapshot":
                snapshot_count += 1
                active_start = int(row["active_start_ns"])
                active_end = int(row["active_end_ns"])
                if active_start != last_active_end:
                    raise RuntimeError(f"{path}: active intervals do not tile")
                active_duration = int(row["active_duration_ns"])
                if active_duration != active_end - active_start:
                    raise RuntimeError(f"{path}: active interval duration differs")
                active_ns += active_duration
                last_active_end = active_end
            elif int(row["active_position_ns"]) != last_active_end:
                raise RuntimeError(f"{path}: gap active position differs")
            for event in row["events"]:
                if int(event["sequence"]) != next_event_sequence:
                    raise RuntimeError(f"{path}: event sequence is not contiguous")
                next_event_sequence += 1
                events += 1
                route_bound += event["event"] == "route-bound"
                if event["event"] == "flow-queued":
                    flow_bytes[event["direction"]] += int(event["bytes"])
    if descriptor is None or final is None:
        raise RuntimeError(f"{path}: descriptor/final closure failed")
    if final["summary"] != summary:
        raise RuntimeError(f"{path}: final summary differs from summary.json")
    if last_wall_end != int(summary["makespan_ns"]):
        raise RuntimeError(f"{path}: wall intervals do not end at makespan")
    if active_ns != int(summary["timeline_active_ns"]):
        raise RuntimeError(f"{path}: active intervals do not reconcile")
    if last_active_end != int(summary["timeline_active_ns"]):
        raise RuntimeError(f"{path}: final active position does not reconcile")
    if events != int(final["event_count"]):
        raise RuntimeError(f"{path}: event count does not reconcile")
    expected_route_events = EXPECTED_JOBS if static else 0
    if route_bound != expected_route_events:
        raise RuntimeError(f"{path}: route-bound event count differs")
    reconciled_flow_bytes = {
        direction: flow_bytes[direction] for direction in ("c_to_f", "f_to_c")
    }
    if reconciled_flow_bytes != {
        "c_to_f": int(summary["c_to_f_bytes"]),
        "f_to_c": int(summary["f_to_c_bytes"]),
    }:
        raise RuntimeError(f"{path}: queued flow bytes do not reconcile")
    return {
        "timeline_records": interval_count,
        "timeline_snapshots": snapshot_count,
        "events": events,
        "route_bound_events": route_bound,
    }


def verify_cell(
    output: Path,
    policy: str,
    codec: str,
    binary: Path,
) -> dict[str, object]:
    width = policy_width(policy)
    cell = output / "cells" / policy / codec
    scenario = scenario_path(output, policy)
    ledger = cell / "ledger.jsonl"
    run = cell / "simulation"
    ledger_result = verify_ledger(ledger, scenario, codec, binary)
    summary = json.loads((run / "summary.json").read_text())
    if not summary["physical_codec_result"] or summary["codec_adapter"] != codec:
        raise RuntimeError(f"{policy}/{codec}: simulator result is not physical")
    if summary["scenario_sha256"] != sha256(scenario):
        raise RuntimeError(f"{policy}/{codec}: simulator scenario digest differs")
    final = ledger_result["final"]["totals"]
    if (
        int(summary["jobs"]) != EXPECTED_JOBS
        or int(summary["c_to_f_bytes"]) != int(final["c_to_f_bytes"])
        or int(summary["f_to_c_bytes"]) != int(final["f_to_c_bytes"])
    ):
        raise RuntimeError(f"{policy}/{codec}: simulator and ledger totals differ")
    assignment_rows = read_tsv(run / "assignments.tsv")
    if len(assignment_rows) != EXPECTED_JOBS:
        raise RuntimeError(f"{policy}/{codec}: assignment count differs")
    by_build: dict[int, dict[int, int]] = defaultdict(dict)
    for row in assignment_rows:
        key = (int(row["build"]), int(row["logical"]))
        actual = (int(row["worker"]), int(row["tu_seq"]), int(row["rel_seq"]))
        if ledger_result["assignments"].get(key) != actual:
            raise RuntimeError(f"{policy}/{codec}: assignment drift at {key}")
        by_build[key[0]][key[1]] = actual[0]
    if set(by_build) != set(range(EXPECTED_BUILDS)):
        raise RuntimeError(f"{policy}/{codec}: build assignment coverage differs")
    populated = set(ledger_result["populated_workers"])
    routing = summary["routing"]
    if width is None:
        if routing["binding"] != "dispatch-time" or len(populated) != 20:
            raise RuntimeError(f"{policy}/{codec}: round-robin control shape differs")
        if by_build[0] == by_build[1]:
            raise RuntimeError(
                f"{policy}/{codec}: round-robin unexpectedly became sticky"
            )
    else:
        if (
            routing["algorithm"] != "stable-rendezvous-v1"
            or int(routing["dense_frontier_workers"]) != width
            or routing["build_number_in_identity"] is not False
        ):
            raise RuntimeError(f"{policy}/{codec}: stable-routing metadata differs")
        for build in range(1, EXPECTED_BUILDS):
            if by_build[build] != by_build[0]:
                raise RuntimeError(f"{policy}/{codec}: stable destination changed")
        homes = set(routing["home_sets"][0]["workers"])
        if len(homes) != width or populated != homes:
            raise RuntimeError(
                f"{policy}/{codec}: populated Fs differ from home frontier"
            )
    jsonl_result = verify_jsonl(
        run / "experiment.jsonl", summary, static=width is not None
    )
    builds = read_tsv(run / "builds.tsv")
    if len(builds) != EXPECTED_BUILDS:
        raise RuntimeError(f"{policy}/{codec}: build timing rows differ")
    result = {
        "schema": "icecream-firefox-static-routing-cell-v1",
        "policy": policy,
        "dense_frontier_workers": width,
        "codec": codec,
        "scenario": str(scenario),
        "scenario_sha256": sha256(scenario),
        "ledger": str(ledger),
        "ledger_sha256": sha256(ledger),
        "experiment_jsonl": str(run / "experiment.jsonl"),
        "experiment_jsonl_sha256": sha256(run / "experiment.jsonl"),
        "codec_binary_sha256": sha256(binary),
        "jobs": EXPECTED_JOBS,
        "populated_workers": sorted(populated),
        "populated_worker_count": len(populated),
        "c_to_f_bytes": int(summary["c_to_f_bytes"]),
        "f_to_c_bytes": int(summary["f_to_c_bytes"]),
        "makespan_ns": int(summary["makespan_ns"]),
        "summed_generation_ns": int(summary["summed_generation_ns"]),
        "timeline_active_ns": int(summary["timeline_active_ns"]),
        "build_bytes": ledger_result["build_bytes"],
        "phase_bytes": ledger_result["phase_bytes"],
        "build_timings": [
            {
                "build": int(row["build"]),
                "input_ready_elapsed_ns": int(row["input_ready_elapsed_ns"]),
                "elapsed_from_release_ns": int(row["elapsed_from_release_ns"]),
                "finish_ns": int(row["finish_ns"]),
            }
            for row in builds
        ],
        "checks": {
            "reconstruction": "pass",
            "ledger_byte_closure": "pass",
            "simulator_byte_closure": "pass",
            "time_closure": "pass",
            "route_closure": "pass",
            "stable_rebuild_mapping": "not-applicable" if width is None else "pass",
            "jsonl_closure": "pass",
            **jsonl_result,
        },
    }
    return result


def run_cell(
    output: Path,
    policy: str,
    codec: str,
    binary: Path,
) -> dict[str, object]:
    cell = output / "cells" / policy / codec
    summary_path = cell / "cell-summary.json"
    if summary_path.exists():
        result = verify_cell(output, policy, codec, binary)
        if json.loads(summary_path.read_text()) != result:
            raise RuntimeError(f"{policy}/{codec}: retained cell summary differs")
        print(f"SKIP verified {policy}/{codec}", flush=True)
        return result
    if cell.exists() and any(cell.iterdir()):
        raise RuntimeError(f"refusing incomplete cell directory: {cell}")
    cell.mkdir(parents=True, exist_ok=True)
    scenario = scenario_path(output, policy)
    work = cell / "codec-work"
    ledger = cell / "ledger.jsonl"
    if codec == "p29":
        builder = [
            sys.executable,
            str(HERE / "build_p29_ledger.py"),
            str(scenario),
            "--codec",
            str(binary.resolve()),
            "--work",
            str(work),
            "--out",
            str(ledger),
        ]
    else:
        builder = [
            sys.executable,
            str(HERE / "build_grz_ledger.py"),
            str(scenario),
            "--codec",
            str(binary.resolve()),
            "--prefix-stride",
            str(EXPECTED_TUS_PER_BUILD),
            "--work",
            str(work),
            "--out",
            str(ledger),
        ]
    print(f"START builder {policy}/{codec}", flush=True)
    checked_timed_run(builder, cell / "builder")
    simulator = [
        sys.executable,
        str(HERE / "run_scenario.py"),
        str(scenario),
        "--codec",
        codec,
        "--ledger",
        str(ledger),
        "--require-payload",
        "--out",
        str(cell / "simulation"),
    ]
    print(f"START simulator {policy}/{codec}", flush=True)
    checked_timed_run(simulator, cell / "simulator")
    result = verify_cell(output, policy, codec, binary)
    write_once(summary_path, canonical_bytes(result))
    print(
        f"PASS {policy}/{codec} c_to_f={result['c_to_f_bytes']} "
        f"f_to_c={result['f_to_c_bytes']} makespan_ns={result['makespan_ns']}",
        flush=True,
    )
    return result


def run_cells(
    output: Path,
    policies: Iterable[str],
    codec: str,
    binary: Path,
    jobs: int,
) -> None:
    selected = list(policies)
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(run_cell, output, policy, codec, binary): policy
            for policy in selected
        }
        for future in as_completed(futures):
            policy = futures[future]
            try:
                future.result()
            except Exception as error:
                print(f"FAIL {policy}/{codec}: {error}", flush=True)
                raise


def replay_cell(
    output: Path,
    policy: str,
    codec: str,
    binary: Path,
) -> dict[str, object]:
    """Replay one already-verified physical ledger with the current simulator."""

    cell = output / "cells" / policy / codec
    ledger = cell / "ledger.jsonl"
    summary_path = cell / "cell-summary.json"
    simulation = cell / "simulation"
    simulator_prefix = cell / "simulator"
    if summary_path.exists():
        result = verify_cell(output, policy, codec, binary)
        if json.loads(summary_path.read_text()) != result:
            raise RuntimeError(f"{policy}/{codec}: retained cell summary differs")
        print(f"SKIP verified {policy}/{codec}", flush=True)
        return result
    if not ledger.is_file():
        raise RuntimeError(f"{policy}/{codec}: physical ledger is absent: {ledger}")
    if simulation.exists() or any(
        simulator_prefix.with_suffix(suffix).exists()
        for suffix in (".command.json", ".time", ".stdout", ".stderr")
    ):
        raise RuntimeError(
            f"{policy}/{codec}: refusing to overwrite an incomplete simulator replay"
        )
    verify_ledger(ledger, scenario_path(output, policy), codec, binary)
    command = [
        sys.executable,
        str(HERE / "run_scenario.py"),
        str(scenario_path(output, policy)),
        "--codec",
        codec,
        "--ledger",
        str(ledger),
        "--require-payload",
        "--out",
        str(simulation),
    ]
    print(f"START simulator {policy}/{codec}", flush=True)
    checked_timed_run(command, simulator_prefix)
    result = verify_cell(output, policy, codec, binary)
    write_once(summary_path, canonical_bytes(result))
    print(
        f"PASS {policy}/{codec} c_to_f={result['c_to_f_bytes']} "
        f"f_to_c={result['f_to_c_bytes']} makespan_ns={result['makespan_ns']}",
        flush=True,
    )
    return result


def replay_cells(
    output: Path,
    policies: Iterable[str],
    codec: str,
    binary: Path,
    jobs: int,
) -> None:
    selected = list(policies)
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(replay_cell, output, policy, codec, binary): policy
            for policy in selected
        }
        for future in as_completed(futures):
            policy = futures[future]
            try:
                future.result()
            except Exception as error:
                print(f"FAIL {policy}/{codec}: {error}", flush=True)
                raise


def decimal_mb(value: int) -> str:
    return f"{value / 1_000_000:.3f}"


def seconds(value: int) -> str:
    return f"{value / 1_000_000_000:.6f}"


def report(output: Path, binaries: dict[str, Path]) -> None:
    rows = []
    for policy, width in POLICIES:
        for codec in CODECS:
            result = verify_cell(output, policy, codec, binaries[codec])
            summary_path = output / "cells" / policy / codec / "cell-summary.json"
            if (
                not summary_path.exists()
                or json.loads(summary_path.read_text()) != result
            ):
                raise RuntimeError(f"{policy}/{codec}: cell summary absent or stale")
            rows.append(result)
    matrix = {
        "schema": "icecream-firefox-static-routing-sweep-result-v1",
        "manifest_sha256": sha256(output / "experiment-manifest.json"),
        "cells": rows,
    }
    write_once(output / "matrix.json", canonical_bytes(matrix))
    fields = [
        "policy",
        "dense_frontier_workers",
        "populated_worker_count",
        "codec",
        "c_to_f_bytes",
        "f_to_c_bytes",
        "cold_c_to_f_bytes",
        "warm1_c_to_f_bytes",
        "warm2_c_to_f_bytes",
        "warm3_c_to_f_bytes",
        "warm4_c_to_f_bytes",
        "makespan_ns",
        "summed_generation_ns",
        "ledger_sha256",
        "experiment_jsonl_sha256",
    ]
    matrix_rows = []
    for row in rows:
        matrix_rows.append(
            {
                "policy": row["policy"],
                "dense_frontier_workers": ""
                if row["dense_frontier_workers"] is None
                else row["dense_frontier_workers"],
                "populated_worker_count": row["populated_worker_count"],
                "codec": row["codec"],
                "c_to_f_bytes": row["c_to_f_bytes"],
                "f_to_c_bytes": row["f_to_c_bytes"],
                **{
                    ("cold" if build == 0 else f"warm{build}") + "_c_to_f_bytes": row[
                        "build_bytes"
                    ][str(build)]["c_to_f_bytes"]
                    for build in range(EXPECTED_BUILDS)
                },
                "makespan_ns": row["makespan_ns"],
                "summed_generation_ns": row["summed_generation_ns"],
                "ledger_sha256": row["ledger_sha256"],
                "experiment_jsonl_sha256": row["experiment_jsonl_sha256"],
            }
        )
    matrix_tsv = output / "matrix.tsv"
    if matrix_tsv.exists():
        raise RuntimeError(f"refusing to overwrite retained report: {matrix_tsv}")
    with matrix_tsv.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(matrix_rows)

    controls = {row["codec"]: row for row in rows if row["policy"] == "round-robin"}
    lines = [
        "# Exact Firefox static-routing sweep",
        "",
        "This is the corrected 2,498-TU Firefox workload, one cold plus four unchanged warm",
        "builds, twenty available Fs, 200 compiler slots and 400 input-staging slots per F,",
        "and one aggregate 1-Gbit/s C uplink. All TUs are released at generation start; no",
        "producer timing is modeled. Compiler environments are resident.",
        "",
        "Each cell has its own real-codec physical ledger and common-simulator JSONL replay.",
        "All cells passed reconstruction, directional byte closure, route/TU/REL closure,",
        "JSONL event closure, and simulated-time closure.",
        "",
        "| policy | populated Fs | codec | cold MB | warm 1 MB | warm 2 MB | warm 3 MB | warm 4 MB | total C→F MB | F→C MB | generation sum s | versus round-robin bytes |",
        "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        control = controls[row["codec"]]
        reduction = 1 - row["c_to_f_bytes"] / control["c_to_f_bytes"]
        builds = row["build_bytes"]
        lines.append(
            "| {policy} | {workers} | {codec} | {b0} | {b1} | {b2} | {b3} | {b4} | "
            "{total} | {reverse} | {time} | {reduction:+.2%} |".format(
                policy=row["policy"],
                workers=row["populated_worker_count"],
                codec=row["codec"].upper(),
                b0=decimal_mb(builds["0"]["c_to_f_bytes"]),
                b1=decimal_mb(builds["1"]["c_to_f_bytes"]),
                b2=decimal_mb(builds["2"]["c_to_f_bytes"]),
                b3=decimal_mb(builds["3"]["c_to_f_bytes"]),
                b4=decimal_mb(builds["4"]["c_to_f_bytes"]),
                total=decimal_mb(row["c_to_f_bytes"]),
                reverse=decimal_mb(row["f_to_c_bytes"]),
                time=seconds(row["summed_generation_ns"]),
                reduction=reduction,
            )
        )
    lines.extend(
        [
            "",
            "## Physical phase breakdown",
            "",
            "All values below are decimal MB over the cold build plus four warm builds.",
            "A dash means that the codec has no such phase.",
            "",
            "| policy | codec | Root | LINES | Fill | close | Need | Ack | GRZ frame |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    phase_columns = (
        ("c_to_f:p29-root", "p29"),
        ("c_to_f:p29-lines", "p29"),
        ("c_to_f:p29-fill", "p29"),
        ("c_to_f:p29-close", "p29"),
        ("f_to_c:p29-need", "p29"),
        ("f_to_c:p29-ack", "p29"),
        ("c_to_f:grz-current-tu-frame", "grz"),
    )
    for row in rows:
        values = []
        for phase, owner in phase_columns:
            values.append(
                decimal_mb(row["phase_bytes"].get(phase, 0))
                if row["codec"] == owner
                else "—"
            )
        lines.append(
            "| {policy} | {codec} | {values} |".format(
                policy=row["policy"],
                codec=row["codec"].upper(),
                values=" | ".join(values),
            )
        )
    lines.extend(
        [
            "",
            "## Interpretation boundary",
            "",
            "The time columns cover exact source-dialogue network behavior plus the existing",
            "compile-duration trace. They still omit F decode/install/materialize/verify and",
            "compiler-pipe costs, compiled-result return traffic, optional environment setup,",
            "and restart/eviction events. The byte sweep is therefore the decision input for",
            "which frontier widths deserve those next timing stages, not an end-to-end claim.",
            "",
            "## Retained evidence",
            "",
            f"- Experiment manifest: `{output / 'experiment-manifest.json'}`",
            f"- Machine-readable matrix: `{output / 'matrix.json'}`",
            f"- Flat matrix: `{output / 'matrix.tsv'}`",
            f"- Per-cell scenarios, ledgers, logs, JSONL, summaries, TSVs, and HTML: `{output / 'cells'}`",
            "",
        ]
    )
    write_once(output / "FIREFOX-STATIC-ROUTING-SWEEP.md", "\n".join(lines).encode())
    print(output / "FIREFOX-STATIC-ROUTING-SWEEP.md")


def parse_binaries(args: argparse.Namespace) -> dict[str, Path]:
    return {"p29": args.p29_codec.resolve(), "grz": args.grz_codec.resolve()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--corpus-root", type=Path, required=True)
    parser.add_argument("--p29-codec", type=Path, required=True)
    parser.add_argument("--grz-codec", type=Path, required=True)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    subparsers.add_parser("prepare")
    cell_parser = subparsers.add_parser("cell")
    cell_parser.add_argument(
        "--policy", choices=[name for name, _ in POLICIES], required=True
    )
    cell_parser.add_argument("--codec", choices=CODECS, required=True)
    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--codec", choices=CODECS, required=True)
    run_parser.add_argument("--jobs", type=int, default=1)
    run_parser.add_argument(
        "--policy", action="append", choices=[name for name, _ in POLICIES]
    )
    replay_parser = subparsers.add_parser(
        "replay", help="simulate already-retained exact physical ledgers"
    )
    replay_parser.add_argument("--codec", choices=CODECS, required=True)
    replay_parser.add_argument("--jobs", type=int, default=1)
    replay_parser.add_argument(
        "--policy", action="append", choices=[name for name, _ in POLICIES]
    )
    subparsers.add_parser("report")
    args = parser.parse_args()
    output = args.out.resolve()
    corpus_root = args.corpus_root.resolve()
    binaries = parse_binaries(args)
    prepare(output, corpus_root, binaries)
    if args.operation == "prepare":
        print(output / "experiment-manifest.json")
    elif args.operation == "cell":
        run_cell(output, args.policy, args.codec, binaries[args.codec])
    elif args.operation == "run":
        if args.jobs <= 0:
            raise ValueError("--jobs must be positive")
        selected = args.policy or [name for name, _ in POLICIES]
        run_cells(output, selected, args.codec, binaries[args.codec], args.jobs)
    elif args.operation == "replay":
        if args.jobs <= 0:
            raise ValueError("--jobs must be positive")
        selected = args.policy or [name for name, _ in POLICIES]
        replay_cells(output, selected, args.codec, binaries[args.codec], args.jobs)
    else:
        report(output, binaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
