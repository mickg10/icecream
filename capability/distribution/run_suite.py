#!/usr/bin/env python3
"""Run a set of topology scenarios through the one common simulator core."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import time
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("run_scenario.py")
SPEC = importlib.util.spec_from_file_location("distribution_run_scenario", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {MODULE_PATH}")
sim = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sim
SPEC.loader.exec_module(sim)

ADAPTERS = {
    "compile-only": sim.CompileOnlyAdapter,
    "raw": sim.RawAdapter,
}


def load_suite(path: Path) -> tuple[dict[str, object], list[Path]]:
    path = path.resolve()
    document = json.loads(path.read_text())
    if (
        not isinstance(document, dict)
        or document.get("schema") != "icecream-distribution-suite-v1"
    ):
        raise ValueError(f"{path}: unknown suite schema")
    name = document.get("name")
    scenario_values = document.get("scenarios")
    codec_values = document.get("diagnostic_codecs")
    if not isinstance(name, str) or not name:
        raise ValueError(f"{path}: suite name is empty")
    if not isinstance(scenario_values, list) or not scenario_values:
        raise ValueError(f"{path}: scenarios must be a nonempty array")
    if len(scenario_values) != len(set(scenario_values)):
        raise ValueError(f"{path}: scenario paths repeat")
    scenarios = []
    for value in scenario_values:
        if not isinstance(value, str) or not value:
            raise ValueError(f"{path}: scenario path is not a string")
        scenario = (path.parent / value).resolve()
        if not scenario.is_file():
            raise ValueError(f"{path}: scenario does not exist: {scenario}")
        scenarios.append(scenario)
    if not isinstance(codec_values, list) or not codec_values:
        raise ValueError(f"{path}: diagnostic_codecs must be a nonempty array")
    unknown = set(codec_values) - set(ADAPTERS)
    if unknown:
        raise ValueError(f"{path}: unknown diagnostic codecs: {sorted(unknown)}")
    return document, scenarios


def run_suite(
    suite_path: Path,
    output_directory: Path,
    codecs: list[str] | None = None,
    corpus_root: Path | None = None,
    require_payload: bool = False,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    suite_path = suite_path.resolve()
    document, scenario_paths = load_suite(suite_path)
    selected_codecs = codecs or list(document["diagnostic_codecs"])
    if not selected_codecs or len(selected_codecs) != len(set(selected_codecs)):
        raise ValueError("selected codecs must be nonempty and unique")
    unknown = set(selected_codecs) - set(ADAPTERS)
    if unknown:
        raise ValueError(f"unknown diagnostic codecs: {sorted(unknown)}")

    output_directory.mkdir(parents=True, exist_ok=True)
    matrix: list[dict[str, object]] = []
    all_builds: list[dict[str, object]] = []
    all_generations: list[dict[str, object]] = []
    runner_timings: list[dict[str, object]] = []
    suite_start = time.perf_counter()
    for scenario_path in scenario_paths:
        overrides = None
        if corpus_root is not None:
            scenario_document = json.loads(scenario_path.read_text())
            overrides = {
                workload: corpus_root.resolve()
                for workload in (
                    job["id"]
                    for job in scenario_document["environments"]["job_selection"][
                        "jobs"
                    ]
                )
            }
        scenario = sim.load_scenario(scenario_path, overrides)
        if require_payload:
            missing = [
                str(item.payload)
                for items in scenario.work_items.values()
                for item in items
                if not item.payload.is_file()
            ]
            if missing:
                raise ValueError(
                    f"{scenario_path}: {len(missing)} payloads are absent; "
                    f"first is {missing[0]}"
                )
        for codec in selected_codecs:
            run_start = time.perf_counter()
            print(f"RUN {scenario.document['name']} codec={codec}", flush=True)
            run_directory = output_directory / str(scenario.document["name"]) / codec
            run_directory.mkdir(parents=True, exist_ok=True)
            result = sim.Simulator(
                scenario,
                ADAPTERS[codec](),
                timeline_spool_path=run_directory / ".timeline-spool.jsonl",
                event_spool_path=run_directory / ".event-spool.jsonl",
            ).run()
            sim.write_result(scenario, result, run_directory)
            wall_seconds = time.perf_counter() - run_start
            row = {
                "scenario": result.summary["scenario"],
                "topology": sim.topology_label(
                    sim.resolved_scenario_document(scenario)
                ),
                "codec": codec,
                "environments": result.summary["environments"],
                "workers": result.summary["workers"],
                "slots_per_worker": result.summary["slots_per_worker"],
                "total_worker_slots": result.summary["total_worker_slots"],
                "cold_builds": result.summary["cold_builds"],
                "warm_builds": result.summary["warm_builds"],
                "jobs": result.summary["jobs"],
                "raw_bytes": result.summary["raw_bytes"],
                "c_to_f_bytes": result.summary["c_to_f_bytes"],
                "f_to_c_bytes": result.summary["f_to_c_bytes"],
                "summed_generation_ns": result.summary["summed_generation_ns"],
                "summed_generation_seconds": result.summary[
                    "summed_generation_seconds"
                ],
                "summed_capacity_floor_ns": result.summary[
                    "summed_capacity_floor_ns"
                ],
                "summed_capacity_floor_seconds": result.summary[
                    "summed_capacity_floor_seconds"
                ],
                "summed_generation_over_capacity_floor": result.summary[
                    "summed_generation_over_capacity_floor"
                ],
                "capacity_floor_efficiency": result.summary[
                    "capacity_floor_efficiency"
                ],
                "wall_makespan_ns": result.summary["makespan_ns"],
                "wall_makespan_seconds": result.summary["makespan_seconds"],
            }
            matrix.append(row)
            runner_timings.append(
                {
                    "scenario": result.summary["scenario"],
                    "codec": codec,
                    "simulator_wall_seconds": wall_seconds,
                }
            )
            for build in result.builds:
                all_builds.append(
                    {"scenario": result.summary["scenario"], "codec": codec, **build}
                )
            for generation in result.generations:
                all_generations.append(
                    {
                        "scenario": result.summary["scenario"],
                        "codec": codec,
                        **generation,
                    }
                )
            print(
                f"PASS {scenario.document['name']} codec={codec} "
                f"jobs={result.summary['jobs']} "
                f"generation_sum_s={result.summary['summed_generation_seconds']:.9f} "
                f"wall_makespan_s={result.summary['makespan_seconds']:.9f} "
                f"runner_s={wall_seconds:.3f}",
                flush=True,
            )
            del result
        del scenario

    sim.write_tsv(output_directory / "matrix.tsv", matrix)
    sim.write_tsv(output_directory / "builds.tsv", all_builds)
    sim.write_tsv(output_directory / "generations.tsv", all_generations)
    sim.write_tsv(output_directory / "runner-timings.tsv", runner_timings)
    suite_summary = {
        "schema": "icecream-distribution-suite-result-v1",
        "suite": document["name"],
        "suite_path": str(suite_path),
        "suite_sha256": sim.sha256(suite_path),
        "scenario_count": len(scenario_paths),
        "codecs": selected_codecs,
        "runs": len(matrix),
        "matrix": matrix,
    }
    (output_directory / "suite-summary.json").write_text(
        json.dumps(suite_summary, indent=2) + "\n"
    )
    print(
        f"SUITE PASS runs={len(matrix)} runner_s={time.perf_counter() - suite_start:.3f}",
        flush=True,
    )
    return matrix, all_builds


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("suite", type=Path)
    parser.add_argument(
        "--codec",
        action="append",
        choices=tuple(ADAPTERS),
        help="repeat to override the suite's diagnostic codecs",
    )
    parser.add_argument("--corpus-root", type=Path)
    parser.add_argument("--require-payload", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    matrix, _ = run_suite(
        args.suite,
        args.out,
        codecs=args.codec,
        corpus_root=args.corpus_root,
        require_payload=args.require_payload,
    )
    print(json.dumps(matrix, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
