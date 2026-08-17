#!/usr/bin/env python3
"""Self-contained complete P29 bounded-literal integration gate."""

from __future__ import annotations

import argparse
import csv
import re
import struct
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], *, expect_success: bool = True) -> str:
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if expect_success != (completed.returncode == 0):
        raise RuntimeError(
            f"unexpected exit {completed.returncode}: {' '.join(command)}\n"
            f"{completed.stdout}"
        )
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--grouped", type=Path, required=True)
    args = parser.parse_args()
    legacy = args.legacy.resolve()
    grouped = args.grouped.resolve()
    if not legacy.is_file() or not grouped.is_file():
        raise FileNotFoundError("codec50 test binary is missing")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        manifest = root / "manifest.txt"
        paths: list[Path] = []
        for tu in range(7):
            path = root / f"unit-{tu}.ii"
            lines = [f'# 1 "unit-{tu}.cc"\n']
            lines.extend(
                f"template <class T> T repeated_identifier_{index % 17}(T value) "
                f"{{ return value + {index % 13}; }}\n"
                for index in range(240 + tu * 13)
            )
            lines.append(f"int changed_tail_{tu} = {tu};\n")
            path.write_text("".join(lines))
            paths.append(path)
        manifest.write_text("".join(f"{path}\n" for path in paths))

        prefix = root / "p29"
        common = [
            "--manifest",
            str(manifest),
            "--z",
            "3",
            "--mixed-regions",
            "--byte-array-lines",
            "--direct-ordinals",
            "--s1-max-chain",
            "1024",
        ]
        baseline = run(
            [str(legacy), *common, "--mixed-dump-prefix", str(prefix)]
        )
        if "byte-exact=OK" not in baseline:
            raise RuntimeError(f"baseline was not exact:\n{baseline}")
        lengths = prefix.with_suffix(".lengths.raw")
        expected_length_bytes = len(paths) * 4 * 4
        if lengths.stat().st_size != expected_length_bytes:
            raise RuntimeError(
                f"length table is {lengths.stat().st_size}, expected {expected_length_bytes}"
            )

        wire = root / "literal-groups.wire"
        curve = root / "curve.tsv"
        components = root / "components.tsv"
        output = run(
            [
                str(grouped),
                *common,
                "--literal-group-prefix",
                str(prefix),
                "--literal-group-tus",
                "3",
                "--literal-group-skip-zstd10",
                "--literal-group-wire",
                str(wire),
                "--curve-tsv",
                str(curve),
                "--component-curve-tsv",
                str(components),
            ]
        )
        if "byte-exact=OK" not in output:
            raise RuntimeError(f"grouped codec was not exact:\n{output}")
        match = re.search(
            r"literal groups: tus_per_group=3 groups=3 workers=1 raw=(\d+) wire=(\d+)",
            output,
        )
        if not match or int(match.group(2)) != wire.stat().st_size:
            raise RuntimeError(f"group wire report differs:\n{output}")

        curve_rows = list(csv.DictReader(curve.open(), delimiter="\t"))
        component_rows = list(csv.DictReader(components.open(), delimiter="\t"))
        if len(curve_rows) != len(paths) or len(component_rows) != len(paths):
            raise RuntimeError("per-TU ledger row count differs")
        for curve_row, component_row in zip(curve_rows, component_rows, strict=True):
            if curve_row["exact"] != "true" or component_row["exact"] != "true":
                raise RuntimeError("non-exact ledger row")
            component_wire = sum(
                int(value)
                for name, value in component_row.items()
                if name.endswith("_wire_bytes")
                and name not in {"wire_bytes", "cumulative_wire_bytes"}
            )
            if component_wire != int(component_row["wire_bytes"]):
                raise RuntimeError("component ledger does not sum to complete wire")

        bad_prefix = root / "bad"
        bad_prefix.with_suffix(".literal.raw").write_bytes(
            prefix.with_suffix(".literal.raw").read_bytes()
        )
        bad_lengths = bytearray(lengths.read_bytes())
        first_literal = struct.unpack_from("<I", bad_lengths, 4)[0]
        struct.pack_into("<I", bad_lengths, 4, first_literal + 1)
        bad_prefix.with_suffix(".lengths.raw").write_bytes(bad_lengths)
        failed = run(
            [
                str(grouped),
                *common,
                "--literal-group-prefix",
                str(bad_prefix),
                "--literal-group-tus",
                "3",
            ],
            expect_success=False,
        )
        if "lengths do not span the raw input" not in failed:
            raise RuntimeError(f"bad length plan failed for the wrong reason:\n{failed}")

    print("P29 bounded literal groups exactness/accounting/rejection PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
