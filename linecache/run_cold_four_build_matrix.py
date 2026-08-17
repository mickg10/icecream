#!/usr/bin/env python3
"""Run P25-P29 cold codecs over four logical retained-state replays."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
from pathlib import Path

from run_complete_tu_matrix import CORPORA, SCHEMAS, sha256, validate_curve


BUILD_REPETITIONS = 4
COLD_SCHEMAS = tuple(row for row in SCHEMAS if row["state"] == "cold")


def canonical_manifest_entries(manifest: Path) -> tuple[str, ...]:
    entries = []
    for raw in manifest.read_text().splitlines():
        value = raw.strip()
        if not value:
            continue
        path = Path(value)
        if not path.is_absolute():
            path = manifest.parent / path
        entries.append(str(path.resolve()))
    if not entries:
        raise ValueError(f"empty manifest: {manifest}")
    return tuple(entries)


def logical_replay_manifest(
    source: Path, repetitions: int = BUILD_REPETITIONS
) -> dict:
    if repetitions <= 0:
        raise ValueError("replay repetitions must be positive")
    entries = canonical_manifest_entries(source)
    return {
        "source_manifest": str(source.resolve()),
        "source_manifest_sha256": sha256(source),
        "replay_mode": "logical-single-load",
        "replay_repetitions": repetitions,
        "build_repetitions": repetitions,
        "tus_per_repetition": len(entries),
        "tus_per_build": len(entries),
        "total_tus": repetitions * len(entries),
    }


def run_one(
    codec: Path,
    output_root: Path,
    schema: dict,
    corpus: tuple[str, str],
    manifest_metadata: dict,
) -> dict:
    name, _ = corpus
    expected_tus = int(manifest_metadata["total_tus"])
    manifest = Path(manifest_metadata["source_manifest"])
    schema_root = output_root / schema["schema"]
    schema_root.mkdir(parents=True, exist_ok=True)
    curve = schema_root / f"{name}.curve.tsv"
    log_path = schema_root / f"{name}.log"
    if curve.is_file() and log_path.is_file():
        log = log_path.read_text()
        result = validate_curve(curve, log, expected_tus)
        return {
            "corpus": name,
            "schema": schema["schema"],
            "status": "reused",
            "build_repetitions": BUILD_REPETITIONS,
            "tus_per_build": manifest_metadata["tus_per_build"],
            **result,
        }

    temporary_curve = curve.with_suffix(curve.suffix + ".tmp")
    command = [
        str(codec),
        "--manifest",
        str(manifest),
        *schema["flags"],
        "--replay-repetitions",
        str(BUILD_REPETITIONS),
        "--entropy-restart-tus",
        str(manifest_metadata["tus_per_build"]),
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
    return {
        "corpus": name,
        "schema": schema["schema"],
        "status": "ran",
        "build_repetitions": BUILD_REPETITIONS,
        "tus_per_build": manifest_metadata["tus_per_build"],
        **result,
    }


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
    selected = [
        row for row in COLD_SCHEMAS if not args.schema or row["schema"] in args.schema
    ]
    unknown = set(args.schema) - {row["schema"] for row in selected}
    if unknown:
        raise ValueError(f"unknown cold schemas: {sorted(unknown)}")
    corpora = [row for row in CORPORA if not args.corpus or row[0] in args.corpus]
    unknown_corpora = set(args.corpus) - {row[0] for row in corpora}
    if unknown_corpora:
        raise ValueError(f"unknown corpora: {sorted(unknown_corpora)}")

    output_root = args.output.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    manifests = {}
    for corpus, directory in corpora:
        source = args.corpus_root.resolve() / directory / "manifest.txt"
        if not source.is_file():
            raise FileNotFoundError(source)
        manifests[corpus] = logical_replay_manifest(source)

    tasks = [(schema, corpus) for schema in selected for corpus in corpora]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_one,
                args.codec.resolve(),
                output_root,
                schema,
                corpus,
                manifests[corpus[0]],
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
        "experiment": "P25-P29 cold retained-state four-repetition matrix",
        "codec": str(args.codec.resolve()),
        "codec_sha256": sha256(args.codec),
        "corpus_root": str(args.corpus_root.resolve()),
        "build_repetitions": BUILD_REPETITIONS,
        "schemas": selected,
        "manifests": manifests,
        "runs": sorted(results, key=lambda row: (row["schema"], row["corpus"])),
    }
    (output_root / "run.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )
    print(f"complete: {len(results)}/{len(tasks)} exact retained-state runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
