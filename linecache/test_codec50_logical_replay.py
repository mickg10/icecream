#!/usr/bin/env python3
"""Prove logical TU replay is byte-identical to a repeated manifest."""

from __future__ import annotations

import argparse
import csv
import subprocess
import tempfile
from pathlib import Path


REPETITIONS = 4
WIRE_COMPONENTS = (
    "root_wire_bytes",
    "block_wire_bytes",
    "path_wire_bytes",
    "framing_wire_bytes",
    "region_control_wire_bytes",
    "region_other_wire_bytes",
    "literal_wire_bytes",
    "array_control_wire_bytes",
    "array_values_wire_bytes",
    "source_control_wire_bytes",
    "source_files_wire_bytes",
    "selector_wire_bytes",
    "blob_wire_bytes",
    "blob_patch_wire_bytes",
    "line_other_wire_bytes",
    "association_wire_bytes",
    "missing_request_wire_bytes",
    "blob_fallback_request_wire_bytes",
    "blob_fallback_reply_wire_bytes",
    "missing_other_wire_bytes",
)


def run_codec(
    codec: Path,
    manifest: Path,
    curve: Path,
    component_curve: Path,
    flags: tuple[str, ...],
    logical: bool,
) -> str:
    command = [str(codec), "--manifest", str(manifest)]
    if logical:
        command.extend(("--replay-repetitions", str(REPETITIONS)))
    command.extend(flags)
    command.extend(
        (
            "--entropy-restart-tus",
            "2",
            "--curve-tsv",
            str(curve),
            "--component-curve-tsv",
            str(component_curve),
        )
    )
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"codec exited {completed.returncode}: {' '.join(command)}\n{completed.stdout}"
        )
    if "byte-exact=OK  TUs=8" not in completed.stdout:
        raise RuntimeError(f"exact replay marker missing\n{completed.stdout}")
    return completed.stdout


def validate_component_curve(path: Path) -> None:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    if len(rows) != 8:
        raise RuntimeError(f"{path}: expected 8 component rows")
    for ordinal, row in enumerate(rows, 1):
        if int(row["tu"]) != ordinal or row["exact"] != "true":
            raise RuntimeError(f"{path}: invalid component row {ordinal}")
        component_wire = sum(int(row[name]) for name in WIRE_COMPONENTS)
        if component_wire != int(row["wire_bytes"]):
            raise RuntimeError(f"{path}: component sum differs at row {ordinal}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--codec", type=Path, required=True)
    args = parser.parse_args()
    codec = args.codec.resolve()
    if not codec.is_file():
        raise FileNotFoundError(codec)

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        first = root / "first.ii"
        second = root / "second.ii"
        first.write_text(
            '# 1 "common.hpp" 1\nint shared_value;\n'
            '# 2 "first.cpp" 2\nint first() { return shared_value; }\n'
        )
        second.write_text(
            '# 1 "common.hpp" 1\nint shared_value;\n'
            '# 2 "second.cpp" 2\nint second() { return shared_value + 1; }\n'
        )
        source_manifest = root / "source.manifest"
        repeated_manifest = root / "repeated.manifest"
        source_entries = (str(first), str(second))
        source_manifest.write_text("".join(f"{entry}\n" for entry in source_entries))
        repeated_manifest.write_text(
            "".join(
                f"{entry}\n"
                for _ in range(REPETITIONS)
                for entry in source_entries
            )
        )

        configurations = {
            "p25": (
                "--z",
                "3",
                "--mixed-regions",
                "--byte-array-lines",
                "--key-map",
                "--s1-max-chain",
                "64",
            ),
            "direct": (
                "--z",
                "3",
                "--mixed-regions",
                "--byte-array-lines",
                "--direct-ordinals",
                "--s1-max-chain",
                "1024",
            ),
        }
        for name, flags in configurations.items():
            physical_curve = root / f"{name}.physical.tsv"
            logical_curve = root / f"{name}.logical.tsv"
            physical_component_curve = root / f"{name}.physical-components.tsv"
            logical_component_curve = root / f"{name}.logical-components.tsv"
            physical_log = run_codec(
                codec,
                repeated_manifest,
                physical_curve,
                physical_component_curve,
                flags,
                logical=False,
            )
            logical_log = run_codec(
                codec,
                source_manifest,
                logical_curve,
                logical_component_curve,
                flags,
                logical=True,
            )
            if physical_curve.read_bytes() != logical_curve.read_bytes():
                raise RuntimeError(f"{name}: physical and logical curves differ")
            validate_component_curve(physical_component_curve)
            validate_component_curve(logical_component_curve)
            if physical_component_curve.read_bytes() != logical_component_curve.read_bytes():
                raise RuntimeError(f"{name}: physical and logical component curves differ")
            if "physical_tus=8" not in physical_log or "physical_tus=2" not in logical_log:
                raise RuntimeError(f"{name}: physical-load accounting differs")
            print(f"{name}: exact logical replay matches physical repetition")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
