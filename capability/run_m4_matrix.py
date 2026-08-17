#!/usr/bin/env python3
"""Build and execute the protocol-50 M4 acceptance matrix.

The runner retains every combined stdout/stderr log, parses only the literal
physical-socket ledger printed by cap_m4_main, and refuses to publish a summary
unless exact replay, frame closure, and min-with-RAW comparisons all hold.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, asdict
from pathlib import Path


CORPORA = (
    ("llvm", "corpus"),
    ("rocksdb", "corpus2"),
    ("duckdb", "corpus3"),
    ("abseil", "corpus4"),
    ("opencv", "corpus5"),
    ("godot", "corpus6"),
    ("fmt", "corpus7"),
    ("spdlog", "corpus8"),
    ("catch2", "corpus9"),
    ("nlohmann-json", "corpus10"),
    ("range-v3", "corpus11"),
    ("eigen", "corpus12"),
    ("re2", "corpus13"),
    ("leveldb", "corpus14"),
    ("simdjson", "corpus15"),
    ("cereal", "corpus16"),
)


@dataclass(frozen=True)
class RunSpec:
    name: str
    corpus: str
    corpus_dir: str
    codec: str
    repetitions: int = 1
    extra: tuple[str, ...] = ()
    max_files: int | None = None


def kv_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def parse_log(text: str) -> dict[str, object]:
    result: dict[str, object] = {
        "components": {},
        "frames": {},
        "repetition_wire": [],
    }
    for line in text.splitlines():
        if line.startswith("RESULT "):
            result.update(kv_fields(line))
        elif line.startswith("FRAME_LEDGER "):
            fields = kv_fields(line)
            result["frame_closure"] = fields.pop("closure", "")
            result["frame_total"] = int(fields.pop("total"))
            frames: dict[str, dict[str, int]] = {}
            for name, value in fields.items():
                size, count = value.split("/", 1)
                frames[name] = {"bytes": int(size), "count": int(count)}
            result["frames"] = frames
        elif line.startswith("COMPONENT "):
            match = re.match(r"COMPONENT\s+(\S+)\s+(.*)", line)
            if not match:
                continue
            fields = kv_fields("X " + match.group(2))
            choices = fields.pop("choices(raw/z1/z3)").split("/")
            result["components"][match.group(1)] = {
                "raw": int(fields["raw"]),
                "z1_candidate": int(fields["z1_candidate"]),
                "z3_candidate": int(fields["z3_candidate"]),
                "selected": int(fields["selected"]),
                "raw_selected": int(choices[0]),
                "z1_selected": int(choices[1]),
                "z3_selected": int(choices[2]),
                "frames": int(fields["frames"]),
            }
        elif line.startswith("REPETITION "):
            match = re.fullmatch(r"REPETITION (\d+) wire=(\d+)", line)
            if match:
                result["repetition_wire"].append(
                    {"ordinal": int(match.group(1)), "wire": int(match.group(2))}
                )
        elif line.startswith("THROUGHPUT "):
            fields = kv_fields(line)
            result["c_transform_gbps"] = float(fields["C_transform"])
            result["f_decode_expand_gbps"] = float(fields["F_decode_expand"])
            result["relationship_seconds"] = float(fields["relationship"].removesuffix("s"))
            result["relationship_gbps"] = float(fields["relationship_rate"])
            result["system_header_reads"] = int(fields["system_header_reads"])
        elif line.startswith("F M4: exact="):
            fields = kv_fields(line)
            result["f_peak_rss_mib"] = float(fields["peak_rss"].removesuffix("MiB"))
            result["evicted"] = int(fields["evicted"])
        elif line.startswith("M4 complete "):
            fields = kv_fields(line)
            result["complete_seconds"] = float(fields["total"].removesuffix("s"))
            result["c_peak_rss_mib"] = float(fields["C_peak"].removesuffix("MiB"))
            result["child_ok"] = fields["child_ok"]

    required = (
        "exact",
        "actual_socket",
        "raw",
        "frame_closure",
        "frame_total",
        "c_transform_gbps",
        "f_decode_expand_gbps",
        "relationship_gbps",
        "c_peak_rss_mib",
        "f_peak_rss_mib",
    )
    missing = [name for name in required if name not in result]
    if missing:
        raise ValueError(f"log lacks fields: {', '.join(missing)}")
    for name in ("physical_tus", "logical_tus", "served", "raw", "actual_socket", "failures"):
        result[name] = int(result[name])
    result["ratio"] = float(result["ratio"])
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def build_binary(source_root: Path, output: Path, cxx: str) -> list[str]:
    command = [
        cxx,
        "-O3",
        "-march=native",
        "-std=c++17",
        "-DICE_LINE_CAP_LOG2=23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        str(source_root / "cap_m4_main.cpp"),
        str(source_root / "cap_codec.cpp"),
        "-o",
        str(output),
        "-lzstd",
    ]
    subprocess.run(command, check=True)
    return command


def run_one(
    binary: Path,
    corpus_root: Path,
    output: Path,
    spec: RunSpec,
    timeout: int,
    resume: bool,
) -> dict[str, object]:
    log_path = output / "logs" / f"{spec.name}.log"
    manifest = corpus_root / spec.corpus_dir / "manifest.txt"
    if not manifest.is_file():
        raise FileNotFoundError(manifest)
    command = [str(binary), "--manifest", str(manifest), "--codec", spec.codec]
    if spec.repetitions != 1:
        command += ["--repetitions", str(spec.repetitions)]
    if spec.max_files is not None:
        command += ["--max-files", str(spec.max_files)]
    command += list(spec.extra)

    started = time.time()
    if resume and log_path.is_file():
        text = log_path.read_text(errors="replace")
    else:
        print(f"RUN {spec.name}: {shlex.join(command)}", flush=True)
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
        text = completed.stdout
        log_path.write_text(
            f"COMMAND {shlex.join(command)}\nEXIT {completed.returncode}\n{text}"
        )
        if completed.returncode != 0:
            raise RuntimeError(f"{spec.name} exited {completed.returncode}; see {log_path}")
    parsed = parse_log(text)
    parsed.update(asdict(spec))
    parsed["command"] = command
    parsed["log"] = str(log_path)
    parsed["log_sha256"] = sha256(log_path)
    parsed["runner_seconds"] = time.time() - started
    if parsed["exact"] != "OK" or parsed["frame_closure"] != "OK":
        raise RuntimeError(f"{spec.name}: exact/ledger gate failed")
    if parsed["frame_total"] != parsed["actual_socket"]:
        raise RuntimeError(f"{spec.name}: physical ledger does not close")
    if parsed["system_header_reads"] != 0 or parsed["child_ok"] != "yes":
        raise RuntimeError(f"{spec.name}: endpoint gate failed")
    return parsed


def standard_specs(selected: set[str], policies: tuple[str, ...]) -> list[RunSpec]:
    return [
        RunSpec(f"standard-{corpus}-{policy}", corpus, directory, policy)
        for corpus, directory in CORPORA
        if corpus in selected
        for policy in policies
    ]


def retained_specs(selected: set[str]) -> list[RunSpec]:
    return [
        RunSpec(f"retained2-{corpus}-best", corpus, directory, "best", repetitions=2)
        for corpus, directory in CORPORA
        if corpus in selected
    ]


def scenario_specs(selected: set[str]) -> list[RunSpec]:
    lookup = dict(CORPORA)
    specs: list[RunSpec] = []
    if "duckdb" in selected:
        specs += [
            RunSpec("scenario-duckdb-restart", "duckdb", lookup["duckdb"], "best", extra=("--restart", "344")),
            RunSpec("scenario-duckdb-latejoin", "duckdb", lookup["duckdb"], "best", extra=("--latejoin", "344")),
            RunSpec("scenario-duckdb-evict", "duckdb", lookup["duckdb"], "best", extra=("--evict", "100")),
            RunSpec("scenario-duckdb-corrupt", "duckdb", lookup["duckdb"], "best", extra=("--corrupt-tu", "344")),
        ]
    if "rocksdb" in selected:
        specs += [
            RunSpec("scenario-rocksdb-restart", "rocksdb", lookup["rocksdb"], "best", extra=("--restart", "311")),
            RunSpec("scenario-rocksdb-latejoin", "rocksdb", lookup["rocksdb"], "best", extra=("--latejoin", "311")),
            RunSpec("scenario-rocksdb-evict", "rocksdb", lookup["rocksdb"], "best", extra=("--evict", "100")),
        ]
    return specs


def validate_cross_rows(rows: list[dict[str, object]], selected: set[str]) -> None:
    index = {(row["corpus"], row["codec"], row["name"].split("-", 1)[0]): row for row in rows}
    for corpus in selected:
        raw = index.get((corpus, "raw", "standard"))
        if not raw:
            continue
        for codec in ("z1", "z3", "best"):
            contender = index.get((corpus, codec, "standard"))
            if contender and contender["actual_socket"] > raw["actual_socket"]:
                raise RuntimeError(
                    f"{corpus}/{codec} regressed versus component-wise RAW fallback"
                )


def write_outputs(output: Path, rows: list[dict[str, object]], build_command: list[str]) -> None:
    json_path = output / "m4-matrix.json"
    summary = {
        "schema": 1,
        "experiment": "protocol-50 expanded M4 physical socket matrix",
        "build_command": build_command,
        "rows": rows,
    }
    json_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    tsv_path = output / "m4-matrix.tsv"
    columns = (
        "name",
        "corpus",
        "codec",
        "scenario",
        "physical_tus",
        "logical_tus",
        "served",
        "raw",
        "actual_socket",
        "ratio",
        "failures",
        "frame_closure",
        "c_transform_gbps",
        "f_decode_expand_gbps",
        "relationship_gbps",
        "c_peak_rss_mib",
        "f_peak_rss_mib",
        "evicted",
        "log",
        "log_sha256",
    )
    with tsv_path.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=columns, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    hashes = output / "SHA256SUMS"
    retained = [json_path, tsv_path, *sorted((output / "logs").glob("*.log"))]
    hashes.write_text("".join(f"{sha256(path)}  {path.relative_to(output)}\n" for path in retained))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus-root", type=Path, default=Path("/tanksmall/scratch/ictmp"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--policies", default="raw,z1,z3,best")
    parser.add_argument("--corpora", default=",".join(name for name, _ in CORPORA))
    parser.add_argument("--suite", choices=("standard", "retained", "scenarios", "all"), default="all")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()

    known = {name for name, _ in CORPORA}
    selected = {name for name in args.corpora.split(",") if name}
    if not selected or selected - known:
        parser.error(f"unknown/empty corpus selection: {sorted(selected - known)}")
    policies = tuple(name for name in args.policies.split(",") if name)
    if not policies or set(policies) - {"raw", "z1", "z3", "best"}:
        parser.error("policies must be raw,z1,z3,best")

    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "logs").mkdir(exist_ok=True)
    source_root = Path(__file__).resolve().parent
    binary = args.binary or (args.output / "cap_m4_main")
    build_command = build_binary(source_root, binary, args.cxx)

    specs: list[RunSpec] = []
    if args.suite in ("standard", "all"):
        specs += standard_specs(selected, policies)
    if args.suite in ("retained", "all"):
        specs += retained_specs(selected)
    if args.suite in ("scenarios", "all"):
        specs += scenario_specs(selected)

    rows: list[dict[str, object]] = []
    for ordinal, spec in enumerate(specs, 1):
        row = run_one(binary, args.corpus_root, args.output, spec, args.timeout, args.resume)
        rows.append(row)
        print(
            f"PASS {ordinal}/{len(specs)} {spec.name}: "
            f"{row['actual_socket']} B, {row['ratio']:.3f}x, "
            f"C/F/e2e={row['c_transform_gbps']:.3f}/"
            f"{row['f_decode_expand_gbps']:.3f}/{row['relationship_gbps']:.3f} GB/s",
            flush=True,
        )
        write_outputs(args.output, rows, build_command)
    validate_cross_rows(rows, selected)
    write_outputs(args.output, rows, build_command)
    print(f"M4 MATRIX PASS: {len(rows)} rows; artifacts={args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
