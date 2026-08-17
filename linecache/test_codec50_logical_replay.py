#!/usr/bin/env python3
"""Prove logical TU replay is byte-identical to a repeated manifest."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


REPETITIONS = 4


def run_codec(
    codec: Path,
    manifest: Path,
    curve: Path,
    flags: tuple[str, ...],
    logical: bool,
) -> str:
    command = [str(codec), "--manifest", str(manifest)]
    if logical:
        command.extend(("--replay-repetitions", str(REPETITIONS)))
    command.extend(flags)
    command.extend(("--entropy-restart-tus", "2", "--curve-tsv", str(curve)))
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
            physical_log = run_codec(
                codec, repeated_manifest, physical_curve, flags, logical=False
            )
            logical_log = run_codec(
                codec, source_manifest, logical_curve, flags, logical=True
            )
            if physical_curve.read_bytes() != logical_curve.read_bytes():
                raise RuntimeError(f"{name}: physical and logical curves differ")
            if "physical_tus=8" not in physical_log or "physical_tus=2" not in logical_log:
                raise RuntimeError(f"{name}: physical-load accounting differs")
            print(f"{name}: exact logical replay matches physical repetition")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
