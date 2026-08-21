#!/usr/bin/env python3
"""Run a set of topology scenarios through the one common simulator core."""

from __future__ import annotations

import argparse
import html
import importlib.util
import json
import subprocess
import sys
import time
from collections import defaultdict
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
PHYSICAL_BUILDERS = {
    "p29": Path(__file__).with_name("build_p29_ledger.py"),
    "grz": Path(__file__).with_name("build_grz_ledger.py"),
}
ALL_CODECS = {*ADAPTERS, *PHYSICAL_BUILDERS}


def named_paths(values: list[str], option: str, allowed: set[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"{option} must be CODEC=PATH")
        codec, path = value.split("=", 1)
        if codec not in allowed or not path or codec in result:
            raise ValueError(f"invalid or repeated {option}: {value!r}")
        result[codec] = Path(path).resolve()
    return result


def named_options(
    values: list[str], option: str, allowed: set[str]
) -> dict[str, list[str]]:
    result: dict[str, list[str]] = defaultdict(list)
    for value in values:
        if "=" not in value:
            raise ValueError(f"{option} must be CODEC=ARG")
        codec, argument = value.split("=", 1)
        if codec not in allowed or not argument:
            raise ValueError(f"invalid {option}: {value!r}")
        result[codec].append(argument)
    return dict(result)


def corpus_arguments(values: list[str]) -> tuple[Path | None, dict[str, Path]]:
    global_roots = [Path(value).resolve() for value in values if "=" not in value]
    if len(global_roots) > 1:
        raise ValueError("only one unqualified --corpus-root PATH is allowed")
    overrides = sim.payload_overrides([value for value in values if "=" in value])
    return (global_roots[0] if global_roots else None), overrides


def checked_builder_run(
    command: list[str], stdout_path: Path, stderr_path: Path
) -> None:
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr)
    if result.returncode:
        tail = stderr_path.read_text(errors="replace")[-4000:]
        raise RuntimeError(
            f"physical-ledger builder exited {result.returncode}: "
            f"{' '.join(command)}\n{tail}"
        )


def build_physical_ledger(
    codec: str,
    scenario_path: Path,
    binary: Path,
    run_directory: Path,
    overrides: dict[str, Path],
    extra_options: list[str],
    grz_prefix_stride: int,
) -> tuple[Path, float]:
    ledger_path = run_directory / f"{codec}-physical-ledger.jsonl"
    work = run_directory / "codec-work"
    command = [
        sys.executable,
        str(PHYSICAL_BUILDERS[codec]),
        str(scenario_path),
        "--codec",
        str(binary),
        "--work",
        str(work),
        "--out",
        str(ledger_path),
    ]
    if codec == "grz":
        command.extend(["--prefix-stride", str(grz_prefix_stride)])
    for workload, root in sorted(overrides.items()):
        command.extend(["--corpus-root", f"{workload}={root}"])
    for argument in extra_options:
        command.append(f"--codec-option={argument}")
    started = time.perf_counter()
    checked_builder_run(
        command,
        run_directory / "builder.stdout",
        run_directory / "builder.stderr",
    )
    return ledger_path, time.perf_counter() - started


def physical_phase_rows(
    scenario_name: str, codec: str, adapter: object
) -> list[dict[str, object]]:
    if not isinstance(adapter, sim.PhysicalLedgerAdapter):
        return []
    totals: dict[tuple[int, str, str, str], list[int]] = defaultdict(lambda: [0, 0])
    for entry in adapter.entries.values():
        for phase in entry.phases:
            temperature = "cold" if entry.build == 0 else "warm"
            total = totals[(entry.build, temperature, phase.name, phase.direction)]
            total[0] += 1
            total[1] += phase.byte_count
    return [
        {
            "scenario": scenario_name,
            "codec": codec,
            "generation": generation,
            "temperature": temperature,
            "phase": phase,
            "direction": direction,
            "transactions": values[0],
            "bytes": values[1],
        }
        for (generation, temperature, phase, direction), values in sorted(totals.items())
    ]


def matrix_row(result: object, codec: str) -> dict[str, object]:
    generations = result.generations
    cold = [row for row in generations if row["temperature"] == "cold"]
    warm = [row for row in generations if row["temperature"] == "warm"]

    def summed(rows: list[dict[str, object]], key: str) -> int:
        return sum(int(row[key]) for row in rows)

    def elapsed(rows: list[dict[str, object]], end: str) -> int:
        return sum(int(row[end]) - int(row["start_ns"]) for row in rows)

    input_ready_ns = elapsed(generations, "last_input_ready_ns")
    compile_complete_ns = elapsed(generations, "last_compile_finish_ns")
    commit_ns = elapsed(generations, "last_transaction_commit_ns")
    c_to_f_floor_ns = summed(generations, "capacity_c_to_f_floor_ns")
    compiler_floor_ns = summed(generations, "capacity_compiler_floor_ns")
    overlap_floor_ns = summed(generations, "capacity_overlap_floor_ns")
    generation_ns = summed(generations, "duration_ns")
    raw_bytes = int(result.summary["raw_bytes"])
    c_to_f_bytes = int(result.summary["c_to_f_bytes"])
    return {
        "scenario": result.summary["scenario"],
        "topology": "",
        "codec": codec,
        "physical_codec_result": result.summary["physical_codec_result"],
        "environments": result.summary["environments"],
        "workers": result.summary["workers"],
        "slots_per_worker": result.summary["slots_per_worker"],
        "total_worker_slots": result.summary["total_worker_slots"],
        "cold_builds": result.summary["cold_builds"],
        "warm_builds": result.summary["warm_builds"],
        "jobs": result.summary["jobs"],
        "raw_bytes": raw_bytes,
        "c_to_f_bytes": c_to_f_bytes,
        "f_to_c_bytes": result.summary["f_to_c_bytes"],
        "raw_per_c_to_f": raw_bytes / c_to_f_bytes if c_to_f_bytes else "",
        "cold_c_to_f_bytes": summed(cold, "c_to_f_bytes"),
        "warm_c_to_f_bytes": summed(warm, "c_to_f_bytes"),
        "cold_generation_ns": summed(cold, "duration_ns"),
        "warm_generation_ns": summed(warm, "duration_ns"),
        "summed_input_ready_elapsed_ns": input_ready_ns,
        "summed_compile_complete_elapsed_ns": compile_complete_ns,
        "summed_transaction_commit_elapsed_ns": commit_ns,
        "summed_generation_ns": generation_ns,
        "summed_generation_seconds": result.summary["summed_generation_seconds"],
        "summed_c_to_f_capacity_floor_ns": c_to_f_floor_ns,
        "summed_compiler_capacity_floor_ns": compiler_floor_ns,
        "summed_capacity_floor_ns": overlap_floor_ns,
        "summed_capacity_floor_seconds": result.summary[
            "summed_capacity_floor_seconds"
        ],
        "excess_over_capacity_floor_ns": generation_ns - overlap_floor_ns,
        "input_ready_floor_efficiency": (
            c_to_f_floor_ns / input_ready_ns if input_ready_ns else ""
        ),
        "compile_floor_efficiency": (
            compiler_floor_ns / compile_complete_ns if compile_complete_ns else ""
        ),
        "summed_generation_over_capacity_floor": result.summary[
            "summed_generation_over_capacity_floor"
        ],
        "capacity_floor_efficiency": result.summary["capacity_floor_efficiency"],
        "wall_makespan_ns": result.summary["makespan_ns"],
        "wall_makespan_seconds": result.summary["makespan_seconds"],
    }


def render_suite_report(
    suite_name: str,
    matrix: list[dict[str, object]],
    generations: list[dict[str, object]],
    phases: list[dict[str, object]],
    runner_timings: list[dict[str, object]],
) -> str:
    def esc(value: object) -> str:
        return html.escape(str(value))

    def seconds(value: object) -> str:
        return f"{int(value) / 1_000_000_000:.6f}"

    def megabytes(value: object) -> str:
        return f"{int(value) / 1_000_000:.3f}"

    def ratio(value: object) -> str:
        return "" if value == "" else f"{float(value):.4f}"

    def percent(value: object) -> str:
        return "" if value == "" else f"{100.0 * float(value):.3f}%"

    matrix_rows = []
    for row in matrix:
        report_path = f"{row['scenario']}/{row['codec']}/report.html"
        matrix_rows.append(
            "<tr>"
            f"<td>{esc(row['topology'])}</td>"
            f"<td>{esc(row['codec'])}</td>"
            f"<td>{esc(row['jobs'])}</td>"
            f"<td>{megabytes(row['raw_bytes'])}</td>"
            f"<td>{megabytes(row['c_to_f_bytes'])}</td>"
            f"<td>{megabytes(row['cold_c_to_f_bytes'])}</td>"
            f"<td>{megabytes(row['warm_c_to_f_bytes'])}</td>"
            f"<td>{ratio(row['raw_per_c_to_f'])}</td>"
            f"<td>{seconds(row['summed_input_ready_elapsed_ns'])}</td>"
            f"<td>{seconds(row['summed_generation_ns'])}</td>"
            f"<td>{seconds(row['summed_c_to_f_capacity_floor_ns'])}</td>"
            f"<td>{seconds(row['summed_compiler_capacity_floor_ns'])}</td>"
            f"<td>{seconds(row['summed_capacity_floor_ns'])}</td>"
            f"<td>{seconds(row['excess_over_capacity_floor_ns'])}</td>"
            f"<td>{percent(row['capacity_floor_efficiency'])}</td>"
            f'<td><a href="{esc(report_path)}">timeline</a></td>'
            "</tr>"
        )

    generation_rows = []
    for row in generations:
        generation_rows.append(
            "<tr>"
            f"<td>{esc(row['scenario'])}</td><td>{esc(row['codec'])}</td>"
            f"<td>{esc(row['generation'])}</td><td>{esc(row['temperature'])}</td>"
            f"<td>{megabytes(row['c_to_f_bytes'])}</td>"
            f"<td>{seconds(row['input_ready_elapsed_ns'])}</td>"
            f"<td>{seconds(row['compile_elapsed_ns'])}</td>"
            f"<td>{seconds(row['transaction_commit_elapsed_ns'])}</td>"
            f"<td>{seconds(row['duration_ns'])}</td>"
            f"<td>{seconds(row['capacity_c_to_f_floor_ns'])}</td>"
            f"<td>{seconds(row['capacity_compiler_floor_ns'])}</td>"
            f"<td>{seconds(row['capacity_overlap_floor_ns'])}</td>"
            f"<td>{percent(row['capacity_floor_efficiency'])}</td>"
            "</tr>"
        )

    phase_rows = []
    for row in phases:
        phase_rows.append(
            "<tr>"
            f"<td>{esc(row['scenario'])}</td><td>{esc(row['codec'])}</td>"
            f"<td>{esc(row['generation'])}</td><td>{esc(row['temperature'])}</td>"
            f"<td>{esc(row['phase'])}</td><td>{esc(row['direction'])}</td>"
            f"<td>{esc(row['transactions'])}</td><td>{megabytes(row['bytes'])}</td>"
            "</tr>"
        )

    timing_rows = []
    for row in runner_timings:
        timing_rows.append(
            "<tr>"
            f"<td>{esc(row['scenario'])}</td><td>{esc(row['codec'])}</td>"
            f"<td>{esc(row['ledger_source'])}</td>"
            f"<td>{float(row['builder_wall_seconds']):.3f}</td>"
            f"<td>{float(row['simulator_wall_seconds']):.3f}</td>"
            f"<td>{float(row['total_runner_wall_seconds']):.3f}</td>"
            "</tr>"
        )

    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{esc(suite_name)} — distribution results</title>
<style>
:root {{ color-scheme: dark; font-family: ui-sans-serif,system-ui,sans-serif; background:#08111f; color:#dbe7f5 }}
body {{ margin:0 auto; max-width:1800px; padding:28px }} h1,h2 {{ color:#f5f9ff }}
p {{ max-width:1100px; color:#9fb1c7 }} .table {{ overflow:auto; margin:12px 0 32px }}
table {{ border-collapse:collapse; min-width:100%; font-variant-numeric:tabular-nums; font-size:13px }}
th,td {{ border-bottom:1px solid #25354b; padding:7px 9px; text-align:right; white-space:nowrap }}
th {{ position:sticky; top:0; background:#102039; color:#9bd4ff }} td:first-child,th:first-child {{ text-align:left }}
a {{ color:#61d4ff }} code {{ color:#b9f6ca }}
</style></head><body>
<h1>{esc(suite_name)}</h1>
<p>All active-generation times exclude configured idle gaps. Capacity floors are optimistic:
they include configured link/compiler capacity but omit release timing, propagation, codec CPU,
dependency round trips, and queue order. Exact machine-readable values remain in
<code>matrix.tsv</code>, <code>generations.tsv</code>, <code>physical-phases.tsv</code>, and
<code>suite-summary.json</code>.</p>
<h2>Scenario × codec matrix</h2><div class="table"><table><thead><tr>
<th>topology</th><th>codec</th><th>TUs</th><th>raw MB</th><th>C→F MB</th><th>cold MB</th><th>warm MB</th><th>raw/C→F</th><th>input-ready s</th><th>active s</th><th>link floor s</th><th>compiler floor s</th><th>overlap floor s</th><th>excess s</th><th>floor efficiency</th><th>detail</th>
</tr></thead><tbody>{''.join(matrix_rows)}</tbody></table></div>
<h2>Per generation</h2><div class="table"><table><thead><tr>
<th>scenario</th><th>codec</th><th>generation</th><th>state</th><th>C→F MB</th><th>input-ready s</th><th>compile-complete s</th><th>commit s</th><th>active s</th><th>link floor s</th><th>compiler floor s</th><th>overlap floor s</th><th>floor efficiency</th>
</tr></thead><tbody>{''.join(generation_rows)}</tbody></table></div>
<h2>Exact physical phase bytes</h2><div class="table"><table><thead><tr>
<th>scenario</th><th>codec</th><th>generation</th><th>state</th><th>phase</th><th>direction</th><th>transactions</th><th>MB</th>
</tr></thead><tbody>{''.join(phase_rows) if phase_rows else '<tr><td colspan="8">No physical codec in this suite.</td></tr>'}</tbody></table></div>
<h2>Harness runtime</h2><p>These are observed host-side measurement costs, not simulated transfer or compile time.</p>
<div class="table"><table><thead><tr><th>scenario</th><th>codec</th><th>ledger source</th><th>builder s</th><th>simulator/output s</th><th>total s</th></tr></thead><tbody>{''.join(timing_rows)}</tbody></table></div>
</body></html>"""


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
    corpus_overrides: dict[str, Path] | None = None,
    codec_binaries: dict[str, Path] | None = None,
    physical_ledgers: dict[str, Path] | None = None,
    codec_options: dict[str, list[str]] | None = None,
    allow_compatible_ledgers: bool = False,
    reuse_compatible_ledgers: bool = False,
    grz_prefix_stride: int = 64,
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    suite_path = suite_path.resolve()
    document, scenario_paths = load_suite(suite_path)
    selected_codecs = codecs or list(document["diagnostic_codecs"])
    if not selected_codecs or len(selected_codecs) != len(set(selected_codecs)):
        raise ValueError("selected codecs must be nonempty and unique")
    unknown = set(selected_codecs) - ALL_CODECS
    if unknown:
        raise ValueError(f"unknown codecs: {sorted(unknown)}")
    if corpus_root is not None and corpus_overrides:
        raise ValueError("use either a global corpus_root or per-workload overrides")
    if grz_prefix_stride <= 0:
        raise ValueError("GRZ prefix stride must be positive")
    binary_by_codec = {
        key: value.resolve() for key, value in (codec_binaries or {}).items()
    }
    ledger_by_codec = {
        key: value.resolve() for key, value in (physical_ledgers or {}).items()
    }
    options_by_codec = codec_options or {}
    for mapping, label in (
        (binary_by_codec, "codec binary"),
        (ledger_by_codec, "physical ledger"),
        (options_by_codec, "codec options"),
    ):
        invalid = set(mapping) - set(PHYSICAL_BUILDERS)
        if invalid:
            raise ValueError(f"{label} names nonphysical codecs: {sorted(invalid)}")
    for codec in set(selected_codecs) & set(PHYSICAL_BUILDERS):
        supplied = int(codec in binary_by_codec) + int(codec in ledger_by_codec)
        if supplied != 1:
            raise ValueError(
                f"{codec} requires exactly one codec binary or prebuilt physical ledger"
            )
        if codec in binary_by_codec and not binary_by_codec[codec].is_file():
            raise ValueError(f"{codec} binary is absent: {binary_by_codec[codec]}")
        if codec in ledger_by_codec and not ledger_by_codec[codec].is_file():
            raise ValueError(f"{codec} ledger is absent: {ledger_by_codec[codec]}")
        if codec in ledger_by_codec and options_by_codec.get(codec):
            raise ValueError(f"{codec} codec options cannot change a prebuilt ledger")

    output_directory.mkdir(parents=True, exist_ok=True)
    matrix: list[dict[str, object]] = []
    all_builds: list[dict[str, object]] = []
    all_generations: list[dict[str, object]] = []
    all_phases: list[dict[str, object]] = []
    runner_timings: list[dict[str, object]] = []
    reusable_ledgers = dict(ledger_by_codec)
    payload_digest_cache: dict[Path, tuple[tuple[int, int, int, int, int], str]] = {}
    suite_start = time.perf_counter()
    for scenario_path in scenario_paths:
        overrides: dict[str, Path] = dict(corpus_overrides or {})
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
            builder_wall_seconds = 0.0
            ledger_reused = False
            ledger_source = "none"
            if codec in ADAPTERS:
                adapter = ADAPTERS[codec]()
            else:
                ledger_path = reusable_ledgers.get(codec)
                if ledger_path is None:
                    ledger_path, builder_wall_seconds = build_physical_ledger(
                        codec,
                        scenario_path,
                        binary_by_codec[codec],
                        run_directory,
                        overrides,
                        options_by_codec.get(codec, []),
                        grz_prefix_stride,
                    )
                    ledger_source = "built-for-scenario"
                    if reuse_compatible_ledgers:
                        reusable_ledgers[codec] = ledger_path
                else:
                    ledger_reused = True
                    ledger_source = (
                        "prebuilt" if codec in ledger_by_codec else "suite-reuse"
                    )
                adapter = sim.PhysicalLedgerAdapter(
                    ledger_path,
                    scenario,
                    codec,
                    allow_compatible_scenario=(
                        allow_compatible_ledgers
                        or (reuse_compatible_ledgers and ledger_reused)
                    ),
                    payload_digest_cache=payload_digest_cache,
                )
            simulator_start = time.perf_counter()
            result = sim.Simulator(
                scenario,
                adapter,
                timeline_spool_path=run_directory / ".timeline-spool.jsonl",
                event_spool_path=run_directory / ".event-spool.jsonl",
            ).run()
            sim.write_result(scenario, result, run_directory)
            simulator_wall_seconds = time.perf_counter() - simulator_start
            wall_seconds = time.perf_counter() - run_start
            row = matrix_row(result, codec)
            row["topology"] = sim.topology_label(
                sim.resolved_scenario_document(scenario)
            )
            matrix.append(row)
            all_phases.extend(
                physical_phase_rows(str(result.summary["scenario"]), codec, adapter)
            )
            runner_timings.append(
                {
                    "scenario": result.summary["scenario"],
                    "codec": codec,
                    "physical_ledger": (
                        str(ledger_path) if codec in PHYSICAL_BUILDERS else ""
                    ),
                    "ledger_source": ledger_source,
                    "ledger_reused": ledger_reused,
                    "builder_wall_seconds": builder_wall_seconds,
                    "simulator_wall_seconds": simulator_wall_seconds,
                    "total_runner_wall_seconds": wall_seconds,
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
    if all_phases:
        sim.write_tsv(output_directory / "physical-phases.tsv", all_phases)
    sim.write_tsv(output_directory / "runner-timings.tsv", runner_timings)
    suite_summary = {
        "schema": "icecream-distribution-suite-result-v1",
        "suite": document["name"],
        "suite_path": str(suite_path),
        "suite_sha256": sim.sha256(suite_path),
        "scenario_count": len(scenario_paths),
        "codecs": selected_codecs,
        "physical_codecs": sorted(set(selected_codecs) & set(PHYSICAL_BUILDERS)),
        "compatible_ledger_reuse": reuse_compatible_ledgers,
        "runs": len(matrix),
        "matrix": matrix,
    }
    (output_directory / "suite-summary.json").write_text(
        json.dumps(suite_summary, indent=2) + "\n"
    )
    (output_directory / "report.html").write_text(
        render_suite_report(
            str(document["name"]),
            matrix,
            all_generations,
            all_phases,
            runner_timings,
        )
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
        choices=tuple(sorted(ALL_CODECS)),
        help="repeat to override the suite's codecs",
    )
    parser.add_argument(
        "--codec-binary",
        action="append",
        default=[],
        metavar="CODEC=PATH",
        help="P29 or GRZ binary used to build one exact ledger per scenario",
    )
    parser.add_argument(
        "--physical-ledger",
        action="append",
        default=[],
        metavar="CODEC=PATH",
        help="prebuilt P29 or GRZ ledger; exact-scenario binding is the default",
    )
    parser.add_argument(
        "--codec-option",
        action="append",
        default=[],
        metavar="CODEC=ARG",
        help="repeat for extra builder arguments, for example p29=--foo",
    )
    parser.add_argument(
        "--allow-compatible-ledger",
        action="store_true",
        help=(
            "reuse a prebuilt ledger across timing-only scenario variants; every payload "
            "digest and runtime worker/TU_SEQ/REL_SEQ must still match"
        ),
    )
    parser.add_argument(
        "--reuse-compatible-ledger",
        action="store_true",
        help=(
            "when building physical codecs, build the first scenario once and require later "
            "scenarios to have the same exact inputs and runtime route order"
        ),
    )
    parser.add_argument("--grz-prefix-stride", type=int, default=64, metavar="TUS")
    parser.add_argument(
        "--corpus-root",
        action="append",
        default=[],
        metavar="[WORKLOAD=]PATH",
    )
    parser.add_argument("--require-payload", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    global_corpus_root, workload_roots = corpus_arguments(args.corpus_root)
    matrix, _ = run_suite(
        args.suite,
        args.out,
        codecs=args.codec,
        corpus_root=global_corpus_root,
        require_payload=args.require_payload,
        corpus_overrides=workload_roots,
        codec_binaries=named_paths(
            args.codec_binary, "--codec-binary", set(PHYSICAL_BUILDERS)
        ),
        physical_ledgers=named_paths(
            args.physical_ledger, "--physical-ledger", set(PHYSICAL_BUILDERS)
        ),
        codec_options=named_options(
            args.codec_option, "--codec-option", set(PHYSICAL_BUILDERS)
        ),
        allow_compatible_ledgers=args.allow_compatible_ledger,
        reuse_compatible_ledgers=args.reuse_compatible_ledger,
        grz_prefix_stride=args.grz_prefix_stride,
    )
    print(json.dumps(matrix, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
