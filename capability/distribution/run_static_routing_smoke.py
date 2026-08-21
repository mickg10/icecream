#!/usr/bin/env python3
"""Run the tiny real-codec static-routing acceptance cell."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
SCENARIO = HERE / "fixtures" / "static-routing" / "scenario.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_run(command: list[str], log_prefix: Path) -> None:
    with (
        (log_prefix.with_suffix(".stdout")).open("w") as stdout,
        (log_prefix.with_suffix(".stderr")).open("w") as stderr,
    ):
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
    if result.returncode:
        tail = log_prefix.with_suffix(".stderr").read_text(errors="replace")[-3000:]
        raise RuntimeError(
            f"command exited {result.returncode}: {' '.join(command)}\n{tail}"
        )


def read_rows(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def verify_codec(output: Path, codec: str, expected_binary: Path) -> dict[str, object]:
    ledger = output / f"{codec}-ledger.jsonl"
    run = output / f"{codec}-run"
    ledger_rows = read_rows(ledger)
    descriptor, final = ledger_rows[0], ledger_rows[-1]
    summary = json.loads((run / "summary.json").read_text())
    if descriptor["reconstruction"]["status"] != "pass":
        raise RuntimeError(f"{codec}: reconstruction did not pass")
    binary_sha256 = sha256(expected_binary)
    if descriptor["codec_binary_sha256"] != binary_sha256:
        raise RuntimeError(f"{codec}: ledger names a different codec binary")
    if not summary["physical_codec_result"]:
        raise RuntimeError(f"{codec}: simulator did not label the result physical")
    for direction in ("c_to_f_bytes", "f_to_c_bytes"):
        if summary[direction] != final["totals"][direction]:
            raise RuntimeError(f"{codec}: {direction} does not close to its ledger")
    routing = summary["routing"]
    if (
        routing["algorithm"] != "stable-rendezvous-v1"
        or routing["dense_frontier_workers"] != 2
        or routing["build_number_in_identity"] is not False
    ):
        raise RuntimeError(f"{codec}: static-routing metadata is incomplete")

    with (run / "assignments.tsv").open(newline="") as source:
        assignments = list(csv.DictReader(source, delimiter="\t"))
    by_build = {
        build: {
            int(row["logical"]): int(row["worker"])
            for row in assignments
            if int(row["build"]) == build
        }
        for build in (0, 1)
    }
    if by_build[0] != by_build[1]:
        raise RuntimeError(f"{codec}: unchanged TU identities changed destination")
    homes = set(routing["home_sets"][0]["workers"])
    if len(homes) != 2 or not set(by_build[0].values()) <= homes:
        raise RuntimeError(f"{codec}: assignment escaped its dense frontier")

    experiment = read_rows(run / "experiment.jsonl")
    intervals = experiment[1:-1]
    snapshots = [row for row in intervals if row["record"] == "snapshot"]
    timeline_summary = experiment[-1]["summary"]
    if timeline_summary["makespan_ns"] != summary["makespan_ns"]:
        raise RuntimeError(f"{codec}: JSONL and summary makespans differ")
    if intervals[-1]["wall_end_ns"] != summary["makespan_ns"]:
        raise RuntimeError(f"{codec}: JSONL intervals do not close at the makespan")
    if (
        sum(row["active_duration_ns"] for row in snapshots)
        != summary["timeline_active_ns"]
    ):
        raise RuntimeError(f"{codec}: JSONL active-time intervals do not reconcile")
    flow_bytes = {
        direction: sum(
            int(event["bytes"])
            for row in intervals
            for event in row["events"]
            if event["event"] == "flow-queued" and event["direction"] == direction
        )
        for direction in ("c_to_f", "f_to_c")
    }
    if flow_bytes != {
        "c_to_f": summary["c_to_f_bytes"],
        "f_to_c": summary["f_to_c_bytes"],
    }:
        raise RuntimeError(f"{codec}: JSONL flow bytes do not reconcile")
    route_events = sum(
        event["event"] == "route-bound" for row in intervals for event in row["events"]
    )
    if route_events != 8 or experiment[-1]["record"] != "summary":
        raise RuntimeError(f"{codec}: JSONL route/event closure failed")
    return {
        "codec": codec,
        "jobs": summary["jobs"],
        "home_workers": sorted(homes),
        "c_to_f_bytes": summary["c_to_f_bytes"],
        "f_to_c_bytes": summary["f_to_c_bytes"],
        "makespan_ns": summary["makespan_ns"],
        "summed_generation_ns": summary["summed_generation_ns"],
        "codec_binary_sha256": binary_sha256,
        "ledger_sha256": sha256(ledger),
        "experiment_sha256": sha256(run / "experiment.jsonl"),
        "reconstruction": "pass",
        "byte_reconciliation": "pass",
        "time_reconciliation": "pass",
        "stable_rebuild_routing": "pass",
        "jsonl_route_events": route_events,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--p29-codec", type=Path, required=True)
    parser.add_argument("--grz-codec", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    for name, path in (("P29", args.p29_codec), ("GRZ", args.grz_codec)):
        if not path.is_file():
            raise ValueError(f"{name} codec is absent: {path}")
    output = args.out.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    commands = (
        (
            "p29-build",
            [
                sys.executable,
                str(HERE / "build_p29_ledger.py"),
                str(SCENARIO),
                "--codec",
                str(args.p29_codec.resolve()),
                "--work",
                str(output / "p29-codec"),
                "--out",
                str(output / "p29-ledger.jsonl"),
            ],
        ),
        (
            "p29-run",
            [
                sys.executable,
                str(HERE / "run_scenario.py"),
                str(SCENARIO),
                "--codec",
                "p29",
                "--ledger",
                str(output / "p29-ledger.jsonl"),
                "--out",
                str(output / "p29-run"),
            ],
        ),
        (
            "grz-build",
            [
                sys.executable,
                str(HERE / "build_grz_ledger.py"),
                str(SCENARIO),
                "--codec",
                str(args.grz_codec.resolve()),
                "--work",
                str(output / "grz-codec"),
                "--out",
                str(output / "grz-ledger.jsonl"),
            ],
        ),
        (
            "grz-run",
            [
                sys.executable,
                str(HERE / "run_scenario.py"),
                str(SCENARIO),
                "--codec",
                "grz",
                "--ledger",
                str(output / "grz-ledger.jsonl"),
                "--out",
                str(output / "grz-run"),
            ],
        ),
    )
    for name, command in commands:
        checked_run(command, output / name)
    summary = {
        "schema": "icecream-static-routing-smoke-v1",
        "scenario": str(SCENARIO),
        "scenario_sha256": sha256(SCENARIO),
        "results": [
            verify_codec(output, "p29", args.p29_codec),
            verify_codec(output, "grz", args.grz_codec),
        ],
    }
    (output / "smoke-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
