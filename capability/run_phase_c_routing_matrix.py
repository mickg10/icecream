#!/usr/bin/env python3
"""Run causal R0-R4 assignments through the exact M5 physical transaction.

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


POLICIES = (
    PolicySpec("R0_RR", "r0-roundrobin", "R0_ROUND_ROBIN"),
    PolicySpec("R0_FASTEST", "r0-fastest", "R0_FASTEST"),
    PolicySpec("R1_RESIDENT", "r1-resident", "R1_RESIDENT"),
    PolicySpec("R2_HOME", "r2-home", "R2_HOME"),
    PolicySpec("R3_RENDEZVOUS", "r3-rendezvous", "R3_RENDEZVOUS"),
    PolicySpec("R4_BYTES", "r4-state", "R4_STATE_AWARE"),
    # At 1 Gbit/s, 1 ms of completion delay is equivalent to 125,000 wire bytes.
    PolicySpec(
        "R4_1GBIT_TIME", "r4-state", "R4_STATE_AWARE", (125_000, 1_000_000)
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
    policy: PolicySpec,
    repetitions: int,
    codec: str,
    max_files: int | None,
    timeout: int,
    resume: bool,
    planner_sha: str,
    physical_sha: str,
) -> dict[str, object]:
    tag = f"{corpus.name}.m{width}.{policy.label}.b{repetitions}"
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
        str(width),
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


def write_report(path: Path, rows: list[dict[str, object]], widths: list[int]) -> None:
    by_key = {
        (str(row["corpus"]), int(row["nominal_width"]), str(row["policy"])): row
        for row in rows
    }
    lines = [
        "# Phase C physical routing matrix",
        "",
        "All byte columns below are exact C-to-F socket bytes after byte-exact reconstruction. "
        "The planner estimate chooses the route but is not substituted for the physical score.",
        "",
    ]
    for corpus in CORPORA:
        if not any(str(row["corpus"]) == corpus.name for row in rows):
            continue
        lines += [f"## {corpus.name}", ""]
        for width in widths:
            lines += [
                f"### M={width}",
                "",
                "| policy | cold C→F | warm C→F | N_eff | H_route | model makespan |",
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
                    f"{int(row['estimated_makespan_ns']) / 1e6:.2f} ms |"
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
    parser.add_argument("--corpora", default=",".join(corpus.name for corpus in CORPORA))
    parser.add_argument("--policies", default=",".join(policy.label for policy in POLICIES))
    parser.add_argument("--repetitions", type=int, choices=(1, 2), default=2)
    parser.add_argument("--codec", choices=("z1", "z3"), default="z1")
    parser.add_argument("--max-files", type=int)
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--planner-commit", required=True)
    parser.add_argument("--physical-commit", required=True)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    widths = [int(value) for value in args.widths.split(",") if value]
    if not widths or widths != sorted(set(widths)) or any(not 1 <= value <= 32 for value in widths):
        parser.error("widths must be unique, sorted and in [1,32]")
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
            for policy in selected_policies:
                ordinal += 1
                row = run_cell(
                    planner=args.planner,
                    physical=physical,
                    manifest=manifest,
                    output=args.output,
                    corpus=corpus,
                    width=width,
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
                write_report(args.output / "REPORT.md", rows, widths)
                print(
                    f"EXACT {ordinal}/{total} {corpus.name} M={width} {policy.label}: "
                    f"cold={row['cold_c_to_f']} warm={row['warm_c_to_f']} "
                    f"N_eff={row['n_eff']:.3f}",
                    flush=True,
                )

    provenance = {
        "schema": 1,
        "planner_commit": args.planner_commit,
        "physical_commit": args.physical_commit,
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
    print(f"PHASE C R0-R4 MATRIX PASS rows={len(rows)} artifacts={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
