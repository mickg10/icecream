#!/usr/bin/env python3
"""Run the exact complete-codec P25-P29 per-TU matrix."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import re
import subprocess
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

COMMON = (
    "--z",
    "3",
    "--mixed-regions",
    "--byte-array-lines",
)
COMPRESSED_BLOBS = (
    "--direct-ordinals",
    "--compressed-blobs",
    "--blob-threads",
    "8",
    "--blob-lazy-fallback",
)
P29_BLOB = (
    "--blob-z",
    "9",
    "--blob-zstd-workers",
    "4",
    "--blob-zstd-job-mib",
    "5",
    "--blob-zstd-overlap-log",
    "3",
)


def state_flags(state: str, direct: bool) -> tuple[str, ...]:
    if state == "cold":
        return ("--direct-ordinals",) if direct else ("--key-map",)
    if state not in ("bit0", "bit1"):
        raise ValueError(f"unknown state: {state}")
    half = ("--half-cold-bit", state[-1])
    return ("--direct-ordinals", *half) if direct else half


def schema_rows() -> tuple[dict, ...]:
    rows = []
    stages = (
        (
            "p25",
            "key-map material baseline",
            False,
            ("--s1-max-chain", "64"),
        ),
        (
            "p26",
            "compressed zlib-member recovery",
            True,
            (*COMPRESSED_BLOBS[1:], "--s1-max-chain", "64"),
        ),
        (
            "p27",
            "P26 plus canonical-MO factoring",
            True,
            (*COMPRESSED_BLOBS[1:], "--mo-factor", "--s1-max-chain", "64"),
        ),
        (
            "p28",
            "P27 plus S1 chain 1024",
            True,
            (*COMPRESSED_BLOBS[1:], "--mo-factor", "--s1-max-chain", "1024"),
        ),
        (
            "p29",
            "P28 plus selected blob zstd-9/LDM workers",
            True,
            (
                *COMPRESSED_BLOBS[1:],
                "--mo-factor",
                "--s1-max-chain",
                "1024",
                *P29_BLOB,
            ),
        ),
    )
    for stage, description, direct, flags in stages:
        for state in ("cold", "bit0", "bit1"):
            rows.append(
                {
                    "schema": f"{stage}-{state}",
                    "stage": stage,
                    "state": state,
                    "description": description,
                    "flags": (*COMMON, *state_flags(state, direct), *flags),
                }
            )
    return tuple(rows)


SCHEMAS = schema_rows()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_curve(path: Path, log: str, expected_tus: int) -> dict:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    if len(rows) != expected_tus:
        raise ValueError(f"{path}: expected {expected_tus} rows, found {len(rows)}")
    cumulative_raw = cumulative_wire = 0
    for ordinal, row in enumerate(rows, 1):
        if int(row["tu"]) != ordinal:
            raise ValueError(f"{path}: TU order differs at {ordinal}")
        if row["exact"] != "true":
            raise ValueError(f"{path}: inexact TU {ordinal}")
        cumulative_raw += int(row["raw_bytes"])
        cumulative_wire += int(row["wire_bytes"])
        if int(row["cumulative_raw_bytes"]) != cumulative_raw:
            raise ValueError(f"{path}: raw total differs at TU {ordinal}")
        if int(row["cumulative_wire_bytes"]) != cumulative_wire:
            raise ValueError(f"{path}: wire total differs at TU {ordinal}")
    loaded = re.search(r"loaded\+interned .* TUs=(\d+) raw=(\d+)", log)
    total = re.search(r"byte-exact=OK\s+TUs=(\d+).*?TOTAL=(\d+)", log, re.S)
    if not loaded or not total:
        raise ValueError(f"{path}: complete exact log markers missing")
    if (int(loaded.group(1)), int(total.group(1))) != (expected_tus, expected_tus):
        raise ValueError(f"{path}: log TU count differs")
    if int(loaded.group(2)) != cumulative_raw or int(total.group(2)) != cumulative_wire:
        raise ValueError(f"{path}: curve/log endpoint differs")
    return {
        "tus": expected_tus,
        "raw_bytes": cumulative_raw,
        "wire_bytes": cumulative_wire,
    }


def run_one(
    codec: Path,
    corpus_root: Path,
    output_root: Path,
    schema: dict,
    corpus: tuple[str, str],
) -> dict:
    name, directory = corpus
    manifest = corpus_root / directory / "manifest.txt"
    if not manifest.is_file():
        raise FileNotFoundError(manifest)
    expected_tus = sum(1 for line in manifest.read_text().splitlines() if line.strip())
    schema_root = output_root / schema["schema"]
    schema_root.mkdir(parents=True, exist_ok=True)
    curve = schema_root / f"{name}.curve.tsv"
    log_path = schema_root / f"{name}.log"
    if curve.is_file() and log_path.is_file():
        log = log_path.read_text()
        result = validate_curve(curve, log, expected_tus)
        return {"corpus": name, "schema": schema["schema"], "status": "reused", **result}

    temporary_curve = curve.with_suffix(curve.suffix + ".tmp")
    command = [
        str(codec),
        "--manifest",
        str(manifest),
        *schema["flags"],
        "--curve-tsv",
        str(temporary_curve),
    ]
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    log = "command: " + " ".join(command) + "\n" + completed.stdout
    log_path.write_text(log)
    if completed.returncode:
        raise RuntimeError(
            f"{schema['schema']}/{name}: codec exited {completed.returncode}; {log_path}"
        )
    result = validate_curve(temporary_curve, log, expected_tus)
    temporary_curve.replace(curve)
    return {"corpus": name, "schema": schema["schema"], "status": "ran", **result}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--codec", type=Path, required=True)
    parser.add_argument("--corpus-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--schema", action="append", default=[])
    parser.add_argument("--corpus", action="append", default=[])
    args = parser.parse_args()
    if not args.codec.is_file() or args.jobs <= 0:
        raise ValueError("a codec file and positive job count are required")
    selected = [row for row in SCHEMAS if not args.schema or row["schema"] in args.schema]
    unknown = set(args.schema) - {row["schema"] for row in selected}
    if unknown:
        raise ValueError(f"unknown schemas: {sorted(unknown)}")
    corpora = [row for row in CORPORA if not args.corpus or row[0] in args.corpus]
    unknown_corpora = set(args.corpus) - {row[0] for row in corpora}
    if unknown_corpora:
        raise ValueError(f"unknown corpora: {sorted(unknown_corpora)}")
    args.output.mkdir(parents=True, exist_ok=True)

    tasks = [(schema, corpus) for schema in selected for corpus in corpora]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_one,
                args.codec.resolve(),
                args.corpus_root.resolve(),
                args.output.resolve(),
                schema,
                corpus,
            ): (schema["schema"], corpus[0])
            for schema, corpus in tasks
        }
        for future in concurrent.futures.as_completed(futures):
            schema, corpus = futures[future]
            result = future.result()
            results.append(result)
            print(
                f"{schema}/{corpus}: {result['status']} "
                f"TUs={result['tus']} wire={result['wire_bytes']}",
                flush=True,
            )

    metadata = {
        "codec": str(args.codec.resolve()),
        "codec_sha256": sha256(args.codec),
        "corpus_root": str(args.corpus_root.resolve()),
        "schemas": selected,
        "runs": sorted(results, key=lambda row: (row["schema"], row["corpus"])),
    }
    (args.output / "run.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )
    print(f"complete: {len(results)}/{len(tasks)} exact runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
