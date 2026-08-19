#!/usr/bin/env python3
"""Run causal R0-R4 and bounded R5 assignments through the exact M5 transaction.

The planner's independent-Region byte values are estimates used only to choose a route.
Every published C-to-F/F-to-C value comes from the socket frame ledger after the selected
assignment reconstructs byte-exactly.  This script refuses a row if the physical worker
sequence differs from the assignment file.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import shlex
import subprocess
from dataclasses import dataclass
from pathlib import Path

from run_m5_acceptance import parse_log, read_curve, sha256


@dataclass(frozen=True)
class Corpus:
    name: str
    directory: str


CORPORA = (
    Corpus("llvm", "corpus"),
    Corpus("rocksdb", "corpus2"),
    Corpus("duckdb", "corpus3"),
    Corpus("godot", "corpus6"),
    Corpus("catch2", "corpus9"),
    Corpus("range-v3", "corpus11"),
    Corpus("eigen", "corpus12"),
    Corpus("cereal", "corpus16"),
)


@dataclass(frozen=True)
class PolicySpec:
    label: str
    planner_name: str
    reported_name: str
    time_weight: tuple[int, int] = (0, 1)
    r5_horizon: int = 0
    r5_beam: int = 0


POLICIES = (
    PolicySpec("R0_RR", "r0-roundrobin", "R0_ROUND_ROBIN"),
    PolicySpec("R0_FASTEST", "r0-fastest", "R0_FASTEST"),
    PolicySpec("R1_RESIDENT", "r1-resident", "R1_RESIDENT"),
    PolicySpec("R2_HOME", "r2-home", "R2_HOME"),
    PolicySpec("R3_RENDEZVOUS", "r3-rendezvous", "R3_RENDEZVOUS"),
    PolicySpec("R4_BYTES", "r4-state", "R4_STATE_AWARE"),
    PolicySpec(
        "R4_W03125", "r4-state", "R4_STATE_AWARE", (31_250, 1_000_000)
    ),
    PolicySpec(
        "R4_W0625", "r4-state", "R4_STATE_AWARE", (62_500, 1_000_000)
    ),
    # At 1 Gbit/s, 1 ms of completion delay is equivalent to 125,000 wire bytes.
    PolicySpec(
        "R4_1GBIT_TIME", "r4-state", "R4_STATE_AWARE", (125_000, 1_000_000)
    ),
    PolicySpec(
        "R4_W250", "r4-state", "R4_STATE_AWARE", (250_000, 1_000_000)
    ),
    PolicySpec(
        "R4_W500", "r4-state", "R4_STATE_AWARE", (500_000, 1_000_000)
    ),
    PolicySpec("R5_BYTES", "r5-bounded", "R5_BOUNDED", (0, 1), 4, 64),
    PolicySpec(
        "R5_W015625", "r5-bounded", "R5_BOUNDED", (15_625, 1_000_000), 4, 64
    ),
    PolicySpec(
        "R5_W03125", "r5-bounded", "R5_BOUNDED", (31_250, 1_000_000), 4, 64
    ),
    PolicySpec(
        "R5_W0625", "r5-bounded", "R5_BOUNDED", (62_500, 1_000_000), 4, 64
    ),
    PolicySpec(
        "R5_1GBIT_TIME",
        "r5-bounded",
        "R5_BOUNDED",
        (125_000, 1_000_000),
        4,
        64,
    ),
    PolicySpec(
        "R5_W250", "r5-bounded", "R5_BOUNDED", (250_000, 1_000_000), 4, 64
    ),
    PolicySpec(
        "R5_W500", "r5-bounded", "R5_BOUNDED", (500_000, 1_000_000), 4, 64
    ),
)


def manifest_fingerprint(path: Path, limit: int | None) -> tuple[str, int, int]:
    digest = hashlib.sha256(b"phase-c-routing-manifest-v1\0")
    paths = [Path(line) for line in path.read_text().splitlines() if line]
    if limit is not None:
        paths = paths[:limit]
    raw = 0
    for source in paths:
        encoded = str(source).encode()
        status = source.stat()
        raw += status.st_size
        digest.update(len(encoded).to_bytes(8, "little"))
        digest.update(encoded)
        digest.update(status.st_size.to_bytes(8, "little"))
        digest.update(status.st_mtime_ns.to_bytes(8, "little"))
    return digest.hexdigest(), len(paths), raw


def build_physical(source: Path, output: Path, cxx: str) -> tuple[Path, list[str]]:
    binary = output / "cap_m5"
    command = [
        cxx,
        "-O3",
        "-DNDEBUG",
        "-march=native",
        "-std=c++17",
        "-DICE_LINE_CAP_LOG2=23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        str(source / "cap_m5_main.cpp"),
        str(source / "cap_codec.cpp"),
        "-o",
        str(binary),
        "-lzstd",
        "-pthread",
    ]
    subprocess.run(command, check=True)
    return binary, command


def planner_fields(output: str) -> dict[str, str]:
    lines = [line for line in output.splitlines() if line.startswith("ROUTING_ESTIMATE ")]
    if len(lines) != 1:
        raise ValueError("planner did not emit exactly one ROUTING_ESTIMATE row")
    fields: dict[str, str] = {}
    for token in lines[0].split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    required = {"schema", "policy", "tus", "estimated_c_to_f", "makespan_ns", "N_eff", "H_route"}
    if required - fields.keys():
        raise ValueError(f"planner row lacks {sorted(required - fields.keys())}")
    if fields["schema"] != "independent-region-zstd3-v1":
        raise ValueError("unknown planner estimator schema")
    return fields


def read_assignment(path: Path, expected: int, workers: int) -> list[int]:
    lines = path.read_text().splitlines()
    if not lines or lines[0] != "routing-assignment-v1" or len(lines) != expected + 1:
        raise ValueError(f"{path}: invalid assignment header/length")
    result: list[int] = []
    for expected_ordinal, line in enumerate(lines[1:]):
        fields = line.split()
        if len(fields) != 2 or int(fields[0]) != expected_ordinal:
            raise ValueError(f"{path}: non-contiguous assignment row")
        worker = int(fields[1])
        if not 0 <= worker < workers:
            raise ValueError(f"{path}: worker outside configured range")
        result.append(worker)
    return result


def effective_width(rows: list[dict[str, int]]) -> tuple[float, float, int]:
    by_worker: dict[int, int] = {}
    for row in rows:
        by_worker[row["worker"]] = by_worker.get(row["worker"], 0) + row["raw"]
    total = sum(by_worker.values())
    if not total:
        return 0.0, 0.0, 0
    probabilities = [value / total for value in by_worker.values() if value]
    return (
        1.0 / sum(value * value for value in probabilities),
        -sum(value * math.log2(value) for value in probabilities),
        len(probabilities),
    )


def physical_byte_model_makespan(
    rows: list[dict[str, int]], slots: list[int]
) -> int:
    """Replay the declared 1-Gbit/0.5-GBps model using physical per-TU bytes."""
    egress_ready = [0] * len(slots)
    compiler_ready = [[0] * count for count in slots]
    makespan = 0
    for row in rows:
        lane = min(
            range(len(slots)), key=lambda value: (egress_ready[value], value)
        )
        transfer_ns = (
            row["c_to_f"] * 8_000_000_000 + 1_000_000_000 - 1
        ) // 1_000_000_000
        transfer_finish = egress_ready[lane] + transfer_ns
        egress_ready[lane] = transfer_finish
        worker = row["worker"]
        compiler_slot = min(
            range(slots[worker]),
            key=lambda value: (compiler_ready[worker][value], value),
        )
        compile_ns = (
            row["raw"] * 1_000_000_000 + 500_000_000 - 1
        ) // 500_000_000
        compile_finish = (
            max(transfer_finish, compiler_ready[worker][compiler_slot]) + compile_ns
        )
        compiler_ready[worker][compiler_slot] = compile_finish
        makespan = max(makespan, compile_finish)
    return makespan


def split_sums(rows: list[dict[str, int]], tus_per_build: int) -> tuple[int, int]:
    if len(rows) == tus_per_build:
        return sum(row["c_to_f"] for row in rows), 0
    if len(rows) != tus_per_build * 2:
        raise ValueError("only one- or two-build Phase-C rows are supported")
    return (
        sum(row["c_to_f"] for row in rows[:tus_per_build]),
        sum(row["c_to_f"] for row in rows[tus_per_build:]),
    )


def split_components(
    rows: list[dict[str, int]], tus_per_build: int
) -> tuple[dict[str, int], dict[str, int]]:
    cold_rows = rows[:tus_per_build]
    warm_rows = rows[tus_per_build:]
    fields = ("c_root", "c_fill", "c_control")
    return (
        {field: sum(row[field] for row in cold_rows) for field in fields},
        {field: sum(row[field] for row in warm_rows) for field in fields},
    )


def run_cell(
    *,
    planner: Path,
    physical: Path,
    manifest: Path,
    output: Path,
    corpus: Corpus,
    width: int,
    slots: list[int],
    requested_slots: int,
    policy: PolicySpec,
    repetitions: int,
    codec: str,
    max_files: int | None,
    timeout: int,
    resume: bool,
    planner_sha: str,
    physical_sha: str,
) -> dict[str, object]:
    slot_tag = "-".join(str(value) for value in slots)
    tag = (
        f"{corpus.name}.m{width}.s{slot_tag}.q{requested_slots}."
        f"{policy.label}.b{repetitions}"
    )
    assignment = output / "assignments" / f"{tag}.txt"
    estimate_curve = output / "estimate-curves" / f"{tag}.tsv"
    physical_curve = output / "physical-curves" / f"{tag}.tsv"
    planner_log = output / "logs" / f"{tag}.planner.log"
    physical_log = output / "logs" / f"{tag}.physical.log"
    fingerprint, tus_per_build, raw_per_build = manifest_fingerprint(manifest, max_files)
    total_tus = tus_per_build * repetitions

    planner_command = [
        str(planner),
        "--manifest",
        str(manifest),
        "--workers",
        str(width),
        "--requested-slots",
        str(requested_slots),
        "--egress-lanes",
        str(width),
        "--repetitions",
        str(repetitions),
        "--policy",
        policy.planner_name,
        "--time-weight",
        str(policy.time_weight[0]),
        str(policy.time_weight[1]),
        "--assignment-out",
        str(assignment),
        "--curve-out",
        str(estimate_curve),
    ]
    if slots != [1] * width:
        planner_command += ["--slots", ",".join(str(value) for value in slots)]
    physical_command = [
        str(physical),
        "--manifest",
        str(manifest),
        "--workers",
        str(width),
        "--wave",
        str(width),
        "--repetitions",
        str(repetitions),
        "--codec",
        codec,
        "--real-pipes",
        "--assignment-file",
        str(assignment),
        "--curve-out",
        str(physical_curve),
    ]
    if max_files is not None:
        planner_command += ["--max-files", str(max_files)]
        physical_command += ["--max-files", str(max_files)]
    if policy.r5_horizon:
        planner_command += [
            "--r5-horizon",
            str(policy.r5_horizon),
            "--r5-beam",
            str(policy.r5_beam),
        ]
    header = (
        f"PLANNER_COMMAND {shlex.join(planner_command)}\n"
        f"PHYSICAL_COMMAND {shlex.join(physical_command)}\n"
        f"PLANNER_SHA256 {planner_sha}\n"
        f"PHYSICAL_SHA256 {physical_sha}\n"
        f"MANIFEST_FINGERPRINT {fingerprint}\n"
    )
    reusable = (
        resume
        and planner_log.is_file()
        and physical_log.is_file()
        and assignment.is_file()
        and estimate_curve.is_file()
        and physical_curve.is_file()
        and planner_log.read_text(errors="replace").startswith(header)
        and physical_log.read_text(errors="replace").startswith(header)
    )
    if not reusable:
        planned = subprocess.run(
            planner_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        planner_log.write_text(header + f"EXIT {planned.returncode}\n" + planned.stdout)
        if planned.returncode:
            raise RuntimeError(f"{tag}: planner failed; see {planner_log}")
        measured = subprocess.run(
            physical_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        physical_log.write_text(header + f"EXIT {measured.returncode}\n" + measured.stdout)
        if measured.returncode:
            raise RuntimeError(f"{tag}: physical replay failed; see {physical_log}")

    planner_text = planner_log.read_text(errors="replace")
    physical_text = physical_log.read_text(errors="replace")
    estimate = planner_fields(planner_text)
    if int(estimate["tus"]) != total_tus:
        raise RuntimeError(f"{tag}: planner TU count differs from manifest")
    if estimate["policy"] != policy.reported_name:
        raise RuntimeError(f"{tag}: planner reported a different policy")
    if policy.r5_horizon:
        required_r5 = {
            "r5_lower_c_to_f",
            "r5_lower_makespan_ns",
            "r5_bound",
            "r5_horizon",
            "r5_beam",
            "r5_expanded",
            "r5_pruned",
        }
        if required_r5 - estimate.keys():
            raise RuntimeError(f"{tag}: planner omitted R5 bound fields")
        if int(estimate["r5_horizon"]) != policy.r5_horizon or int(
            estimate["r5_beam"]
        ) != policy.r5_beam:
            raise RuntimeError(f"{tag}: planner used different R5 search limits")
    parsed = parse_log(physical_text)
    rows = read_curve(physical_curve)
    assignments = read_assignment(assignment, total_tus, width)
    if [row["worker"] for row in rows] != assignments:
        raise RuntimeError(f"{tag}: physical F sequence differs from planner assignment")
    if parsed["exact"] != "OK" or parsed["tus"] != total_tus:
        raise RuntimeError(f"{tag}: physical reconstruction/TU count failed")
    for closure in ("frame_closure", "direction_closure", "routing_closure"):
        if parsed[closure] != "OK":
            raise RuntimeError(f"{tag}: {closure} failed")
    if sum(row["c_to_f"] for row in rows) != parsed["c_to_f"]:
        raise RuntimeError(f"{tag}: physical C-to-F curve does not close")
    if sum(row["f_to_c"] for row in rows) != parsed["f_to_c"]:
        raise RuntimeError(f"{tag}: physical F-to-C curve does not close")
    if sum(row["raw"] for row in rows) != raw_per_build * repetitions:
        raise RuntimeError(f"{tag}: physical raw curve differs from manifest")
    n_eff, entropy, opened = effective_width(rows)
    physical_makespan = physical_byte_model_makespan(rows, slots)
    if abs(float(estimate["N_eff"]) - n_eff) > 1e-5:
        raise RuntimeError(f"{tag}: planner and physical N_eff differ")
    if abs(float(estimate["H_route"]) - entropy) > 1e-5:
        raise RuntimeError(f"{tag}: planner and physical H_route differ")
    cold, warm = split_sums(rows, tus_per_build)
    cold_parts, warm_parts = split_components(rows, tus_per_build)
    return {
        "corpus": corpus.name,
        "tus_per_build": tus_per_build,
        "repetitions": repetitions,
        "nominal_width": width,
        "slot_capacities": ",".join(str(value) for value in slots),
        "requested_slots": requested_slots,
        "policy": policy.label,
        "codec": codec,
        "raw": parsed["raw"],
        "c_to_f": parsed["c_to_f"],
        "f_to_c": parsed["f_to_c"],
        "cold_c_to_f": cold,
        "warm_c_to_f": warm,
        "cold_root": cold_parts["c_root"],
        "cold_fill": cold_parts["c_fill"],
        "cold_control": cold_parts["c_control"],
        "warm_root": warm_parts["c_root"],
        "warm_fill": warm_parts["c_fill"],
        "warm_control": warm_parts["c_control"],
        "root": parsed["c_root"],
        "fill": parsed["c_fill"],
        "control": parsed["c_control"],
        "estimated_c_to_f": int(estimate["estimated_c_to_f"]),
        "estimated_makespan_ns": int(estimate["makespan_ns"]),
        "physical_byte_model_makespan_ns": physical_makespan,
        "estimated_lower_c_to_f": int(estimate.get("r5_lower_c_to_f", "0")),
        "estimated_lower_makespan_ns": int(
            estimate.get("r5_lower_makespan_ns", "0")
        ),
        "estimated_bound_kind": estimate.get("r5_bound", "none"),
        "r5_expanded": int(estimate.get("r5_expanded", "0")),
        "r5_pruned": int(estimate.get("r5_pruned", "0")),
        "n_eff": n_eff,
        "route_entropy": entropy,
        "cache_domains_opened": opened,
        "physical_wall_seconds": parsed["wall_seconds"],
        "relationship_gbps": parsed["relationship_gbps"],
        "planner_log": str(planner_log),
        "physical_log": str(physical_log),
        "assignment": str(assignment),
        "estimate_curve": str(estimate_curve),
        "physical_curve": str(physical_curve),
        "manifest_fingerprint": fingerprint,
    }


def write_tsv(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def write_report(
    path: Path,
    rows: list[dict[str, object]],
    widths: list[int],
    planner_commit: str,
    physical_commit: str,
    runner_commit: str,
) -> None:
    by_key = {
        (str(row["corpus"]), int(row["nominal_width"]), str(row["policy"])): row
        for row in rows
    }
    lines = [
        "# Phase C physical routing matrix",
        "",
        "All byte columns below are exact C-to-F socket bytes after byte-exact reconstruction. "
        "The planner estimate chooses the route but is not substituted for the physical score.",
        "The main makespan column replays those physical per-TU bytes through the declared "
        "1-Gbit/s, one-egress-lane-per-F and 0.5-GB/s compiler model.",
        "R5 lower bounds live only in the labelled independent-Region estimator; they do not "
        "bound the physical byte columns and are reported separately.",
        "",
        f"Planner `{planner_commit}` · physical source `{physical_commit}` · "
        f"matrix runner `{runner_commit}`.",
        "",
    ]
    for corpus in CORPORA:
        if not any(str(row["corpus"]) == corpus.name for row in rows):
            continue
        lines += [f"## {corpus.name}", ""]
        for width in widths:
            topology = next(
                (
                    row
                    for row in rows
                    if str(row["corpus"]) == corpus.name
                    and int(row["nominal_width"]) == width
                ),
                None,
            )
            if topology is None:
                continue
            lines += [
                f"### M={width}; F slots={topology['slot_capacities']}; "
                f"requested={topology['requested_slots']}",
                "",
                "| policy | cold C→F | warm C→F | N_eff | H_route | physical-byte model makespan |",
                "|---|---:|---:|---:|---:|---:|",
            ]
            for policy in POLICIES:
                row = by_key.get((corpus.name, width, policy.label))
                if row is None:
                    continue
                lines.append(
                    f"| {policy.label} | {int(row['cold_c_to_f']) / 1e6:.3f} MB | "
                    f"{int(row['warm_c_to_f']) / 1e6:.3f} MB | "
                    f"{float(row['n_eff']):.2f} | {float(row['route_entropy']):.2f} | "
                    f"{int(row['physical_byte_model_makespan_ns']) / 1e6:.2f} ms |"
                )
            r5_rows = [
                by_key[(corpus.name, width, policy.label)]
                for policy in POLICIES
                if policy.r5_horizon
                and (corpus.name, width, policy.label) in by_key
            ]
            if r5_rows:
                lines += [
                    "",
                    "R5 estimator-space bounds:",
                    "",
                    "| policy | feasible bytes | byte lower | feasible makespan | makespan lower | pruned paths |",
                    "|---|---:|---:|---:|---:|---:|",
                ]
                for row in r5_rows:
                    lines.append(
                        f"| {row['policy']} | {int(row['estimated_c_to_f']) / 1e6:.3f} MB | "
                        f"{int(row['estimated_lower_c_to_f']) / 1e6:.3f} MB | "
                        f"{int(row['estimated_makespan_ns']) / 1e6:.2f} ms | "
                        f"{int(row['estimated_lower_makespan_ns']) / 1e6:.2f} ms | "
                        f"{int(row['r5_pruned'])} |"
                    )
            lines += [
                "",
                "Exact direction components:",
                "",
                "| policy | cold root | cold fill | cold control | warm root | warm fill | warm control |",
                "|---|---:|---:|---:|---:|---:|---:|",
            ]
            for policy in POLICIES:
                row = by_key.get((corpus.name, width, policy.label))
                if row is None:
                    continue
                lines.append(
                    f"| {policy.label} | {int(row['cold_root']) / 1e6:.3f} MB | "
                    f"{int(row['cold_fill']) / 1e6:.3f} MB | "
                    f"{int(row['cold_control']) / 1e6:.3f} MB | "
                    f"{int(row['warm_root']) / 1e6:.3f} MB | "
                    f"{int(row['warm_fill']) / 1e6:.3f} MB | "
                    f"{int(row['warm_control']) / 1e6:.3f} MB |"
                )
            lines.append("")
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--planner", type=Path, required=True)
    parser.add_argument("--corpus-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--widths", default="1,4,8,20")
    parser.add_argument("--slots")
    parser.add_argument("--requested-slots", type=int)
    parser.add_argument("--corpora", default=",".join(corpus.name for corpus in CORPORA))
    parser.add_argument("--policies", default=",".join(policy.label for policy in POLICIES))
    parser.add_argument("--repetitions", type=int, choices=(1, 2), default=2)
    parser.add_argument("--codec", choices=("z1", "z3"), default="z1")
    parser.add_argument("--max-files", type=int)
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--planner-commit", required=True)
    parser.add_argument("--physical-commit", required=True)
    parser.add_argument("--runner-commit", required=True)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    widths = [int(value) for value in args.widths.split(",") if value]
    if not widths or widths != sorted(set(widths)) or any(not 1 <= value <= 32 for value in widths):
        parser.error("widths must be unique, sorted and in [1,32]")
    if args.slots:
        if len(widths) != 1:
            parser.error("--slots requires exactly one --widths value")
        try:
            slots = [int(value) for value in args.slots.split(",")]
        except ValueError:
            parser.error("--slots must contain positive integers")
        if len(slots) != widths[0] or any(value < 1 for value in slots):
            parser.error("--slots count must equal width and every value must be positive")
    else:
        slots = [1] * widths[0] if len(widths) == 1 else []
    if args.requested_slots is not None and args.requested_slots < 1:
        parser.error("--requested-slots must be positive")
    if args.slots:
        requested_slots = args.requested_slots or sum(slots)
        if requested_slots > sum(slots):
            parser.error("--requested-slots exceeds supplied capacity")
    elif args.requested_slots is not None:
        if len(widths) != 1:
            parser.error("--requested-slots requires exactly one --widths value")
        requested_slots = args.requested_slots
        if requested_slots > widths[0]:
            parser.error("--requested-slots exceeds one-slot-per-F capacity")
    else:
        requested_slots = 0
    corpus_names = {value for value in args.corpora.split(",") if value}
    policy_names = {value for value in args.policies.split(",") if value}
    selected_corpora = [corpus for corpus in CORPORA if corpus.name in corpus_names]
    selected_policies = [policy for policy in POLICIES if policy.label in policy_names]
    if len(selected_corpora) != len(corpus_names):
        parser.error("unknown corpus name")
    if len(selected_policies) != len(policy_names):
        parser.error("unknown policy name")
    if args.max_files is not None and args.max_files < 1:
        parser.error("--max-files must be positive")
    if not args.planner.is_file():
        parser.error("planner binary does not exist")

    args.output.mkdir(parents=True, exist_ok=True)
    for directory in ("logs", "assignments", "estimate-curves", "physical-curves"):
        (args.output / directory).mkdir(exist_ok=True)
    source = Path(__file__).resolve().parent
    physical, build_command = build_physical(source, args.output, args.cxx)
    planner_sha, physical_sha = sha256(args.planner), sha256(physical)

    rows: list[dict[str, object]] = []
    total = len(selected_corpora) * len(widths) * len(selected_policies)
    ordinal = 0
    for corpus in selected_corpora:
        manifest = args.corpus_root / corpus.directory / "manifest.txt"
        if not manifest.is_file():
            raise RuntimeError(f"missing manifest: {manifest}")
        for width in widths:
            cell_slots = slots if slots else [1] * width
            cell_requested_slots = requested_slots or width
            for policy in selected_policies:
                ordinal += 1
                row = run_cell(
                    planner=args.planner,
                    physical=physical,
                    manifest=manifest,
                    output=args.output,
                    corpus=corpus,
                    width=width,
                    slots=cell_slots,
                    requested_slots=cell_requested_slots,
                    policy=policy,
                    repetitions=args.repetitions,
                    codec=args.codec,
                    max_files=args.max_files,
                    timeout=args.timeout,
                    resume=args.resume,
                    planner_sha=planner_sha,
                    physical_sha=physical_sha,
                )
                rows.append(row)
                write_tsv(args.output / "matrix.tsv", rows)
                write_report(
                    args.output / "REPORT.md",
                    rows,
                    widths,
                    args.planner_commit,
                    args.physical_commit,
                    args.runner_commit,
                )
                print(
                    f"EXACT {ordinal}/{total} {corpus.name} M={width} {policy.label}: "
                    f"cold={row['cold_c_to_f']} warm={row['warm_c_to_f']} "
                    f"N_eff={row['n_eff']:.3f}",
                    flush=True,
                )

    provenance = {
        "schema": 3,
        "planner_commit": args.planner_commit,
        "physical_commit": args.physical_commit,
        "runner_commit": args.runner_commit,
        "runner_sha256": sha256(Path(__file__).resolve()),
        "slots": args.slots or "one-per-F",
        "requested_slots": args.requested_slots or "all",
        "planner_sha256": planner_sha,
        "physical_sha256": physical_sha,
        "physical_build_command": build_command,
        "estimator_schema": "independent-region-zstd3-v1",
        "score": "exact physical C-to-F socket bytes",
        "rows": rows,
    }
    (args.output / "matrix.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n"
    )
    artifacts = sorted(path for path in args.output.rglob("*") if path.is_file())
    (args.output / "SHA256SUMS").write_text(
        "".join(
            f"{sha256(path)}  {path.relative_to(args.output)}\n"
            for path in artifacts
            if path.name != "SHA256SUMS"
        )
    )
    print(f"PHASE C R0-R5 MATRIX PASS rows={len(rows)} artifacts={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
