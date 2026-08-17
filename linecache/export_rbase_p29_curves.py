#!/usr/bin/env python3
"""Publish first-repetition RBASE-P29 rows from retained exact curves."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path

from run_complete_tu_matrix import CORPORA
from run_cold_four_build_matrix import BUILD_REPETITIONS


EXPECTED_RAW = 28_554_671_510
EXPECTED_WIRE = 91_864_787
EXPECTED_TUS = 9_292
FIELDS = (
    "corpus",
    "tu",
    "raw_bytes",
    "wire_bytes",
    "cumulative_raw_bytes",
    "cumulative_wire_bytes",
    "exact",
    "source_curve_sha256",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_first_repetition(path: Path) -> tuple[list[dict], int]:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    if not rows or len(rows) % BUILD_REPETITIONS:
        raise ValueError(f"{path}: curve does not contain four complete repetitions")
    tus = len(rows) // BUILD_REPETITIONS
    first_raw = [int(row["raw_bytes"]) for row in rows[:tus]]
    for ordinal, row in enumerate(rows, 1):
        if int(row["tu"]) != ordinal or row["exact"] != "true":
            raise ValueError(f"{path}: invalid exact row {ordinal}")
        if int(row["raw_bytes"]) != first_raw[(ordinal - 1) % tus]:
            raise ValueError(f"{path}: repeated raw sequence differs at row {ordinal}")
    return rows[:tus], tus


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()

    run_root = args.run_root.resolve()
    output_rows = []
    corpora = []
    aggregate_raw = aggregate_wire = aggregate_tus = 0
    for corpus, _ in CORPORA:
        curve = run_root / "p29-cold" / f"{corpus}.curve.tsv"
        if not curve.is_file():
            raise FileNotFoundError(curve)
        rows, tus = load_first_repetition(curve)
        curve_hash = sha256(curve)
        cumulative_raw = cumulative_wire = 0
        for tu, row in enumerate(rows, 1):
            raw = int(row["raw_bytes"])
            wire = int(row["wire_bytes"])
            cumulative_raw += raw
            cumulative_wire += wire
            if (
                int(row["cumulative_raw_bytes"]) != cumulative_raw
                or int(row["cumulative_wire_bytes"]) != cumulative_wire
            ):
                raise ValueError(f"{curve}: first-repetition cumulative total differs")
            output_rows.append(
                {
                    "corpus": corpus,
                    "tu": tu,
                    "raw_bytes": raw,
                    "wire_bytes": wire,
                    "cumulative_raw_bytes": cumulative_raw,
                    "cumulative_wire_bytes": cumulative_wire,
                    "exact": "true",
                    "source_curve_sha256": curve_hash,
                }
            )
        aggregate_tus += tus
        aggregate_raw += cumulative_raw
        aggregate_wire += cumulative_wire
        corpora.append(
            {
                "corpus": corpus,
                "tus": tus,
                "raw_bytes": cumulative_raw,
                "wire_bytes": cumulative_wire,
                "source_curve": str(curve),
                "source_curve_sha256": curve_hash,
            }
        )

    if (aggregate_tus, aggregate_raw, aggregate_wire) != (
        EXPECTED_TUS,
        EXPECTED_RAW,
        EXPECTED_WIRE,
    ):
        raise ValueError(
            "aggregate retained endpoint differs: "
            f"tus={aggregate_tus} raw={aggregate_raw} wire={aggregate_wire}"
        )

    args.tsv.parent.mkdir(parents=True, exist_ok=True)
    with args.tsv.open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=FIELDS, delimiter="\t")
        writer.writeheader()
        writer.writerows(output_rows)

    summary = {
        "artifact": "RBASE-P29 fixed-16 first-repetition per-TU export",
        "coverage": {
            "projects": len(corpora),
            "tus": aggregate_tus,
            "raw_bytes": aggregate_raw,
            "wire_bytes": aggregate_wire,
        },
        "state_contract": {
            "receiver_dynamic_state_at_tu_0": "empty",
            "object_state": "retained after each exact TU",
            "installed_starting_model": "none",
            "tu_order": "native retained manifest order",
            "mixed_material_entropy": (
                "stateful stream with a TU flush; this first repetition has no "
                "product build signal and is not an independent-frame M4 result"
            ),
        },
        "codec": {
            "schema": "p29-cold",
            "common": ["--z", "3", "--mixed-regions", "--byte-array-lines"],
            "identity": ["--direct-ordinals"],
            "blob": [
                "--compressed-blobs",
                "--blob-threads",
                "8",
                "--blob-lazy-fallback",
                "--mo-factor",
                "--blob-z",
                "9",
                "--blob-zstd-workers",
                "4",
                "--blob-zstd-job-mib",
                "5",
                "--blob-zstd-overlap-log",
                "3",
            ],
            "s1": ["--s1-max-chain", "1024"],
        },
        "tsv": str(args.tsv),
        "tsv_sha256": sha256(args.tsv),
        "corpora": corpora,
    }
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(
        f"wrote {len(output_rows)} exact P29 rows: "
        f"raw={aggregate_raw} wire={aggregate_wire}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
