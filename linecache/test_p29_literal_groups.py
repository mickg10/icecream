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
            lines: list[str] = []
            # Repeated marker-delimited Region sequences force the first group to define and
            # reference S1 Blocks.  The per-TU tail then adds new Regions after that group, so
            # the fixture detects the legacy final-NREG-dependent Block namespace.
            for region in range(12):
                lines.append(f'# 1 "/usr/include/prefix-fixture-{region % 4}.h" 1 3 4\n')
                lines.extend(
                    f"template <class T> T repeated_identifier_{index % 17}(T value) "
                    f"{{ return value + {index % 13}; }}\n"
                    for index in range(24 + region % 3)
                )
            lines.append(f'# 1 "unit-{tu}.cc"\n')
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

        legacy_wire = root / "legacy-literal-groups.wire"
        legacy_curve = root / "legacy-curve.tsv"
        legacy_components = root / "legacy-components.tsv"
        legacy_grouped = run(
            [
                str(legacy),
                *common,
                "--literal-group-prefix",
                str(prefix),
                "--literal-group-tus",
                "3",
                "--literal-group-skip-zstd10",
                "--literal-group-wire",
                str(legacy_wire),
                "--curve-tsv",
                str(legacy_curve),
                "--component-curve-tsv",
                str(legacy_components),
            ]
        )
        if "byte-exact=OK" not in legacy_grouped:
            raise RuntimeError(f"legacy grouped codec was not exact:\n{legacy_grouped}")
        if legacy_wire.read_bytes() != wire.read_bytes():
            raise RuntimeError("opt-in prefix correction changed the legacy literal wire")
        if legacy_curve.read_bytes() != curve.read_bytes():
            raise RuntimeError("opt-in prefix correction changed the legacy curve")
        if legacy_components.read_bytes() != components.read_bytes():
            raise RuntimeError("opt-in prefix correction changed legacy component accounting")

        # Stable Root ids do not depend on Regions introduced by later TUs.  The ordinary
        # structure streams flush after each TU; diagnostic open-final mode omits only the END
        # bytes a standalone run would otherwise add.  A complete run's first group must then
        # match a suffix-blind run byte-for-byte and account-for-account without paying for a
        # new entropy frame at every literal-group boundary.
        stable_wire = root / "stable-full.wire"
        stable_curve = root / "stable-full.tsv"
        stable_components = root / "stable-full-components.tsv"
        stable_output = run(
            [
                str(grouped),
                *common,
                "--literal-group-prefix",
                str(prefix),
                "--literal-group-tus",
                "3",
                "--stable-root-tags",
                "--literal-group-skip-zstd10",
                "--literal-group-wire",
                str(stable_wire),
                "--curve-tsv",
                str(stable_curve),
                "--component-curve-tsv",
                str(stable_components),
            ]
        )
        if "byte-exact=OK" not in stable_output or "stable Root tags" not in stable_output:
            raise RuntimeError(f"stable-Root run was not exact and bound:\n{stable_output}")

        # A program that ends before a full selector group has no unseen suffix.  Its replay
        # must terminate normally (including entropy END bytes), and a second complete run must
        # be identical.  The matrix runner uses this mode for programs with at most 112 TUs.
        stable_repeat_wire = root / "stable-repeat.wire"
        stable_repeat_curve = root / "stable-repeat.tsv"
        stable_repeat_components = root / "stable-repeat-components.tsv"
        stable_repeat_output = run(
            [
                str(grouped),
                *common,
                "--literal-group-prefix",
                str(prefix),
                "--literal-group-tus",
                "3",
                "--stable-root-tags",
                "--literal-group-skip-zstd10",
                "--literal-group-wire",
                str(stable_repeat_wire),
                "--curve-tsv",
                str(stable_repeat_curve),
                "--component-curve-tsv",
                str(stable_repeat_components),
            ]
        )
        if "byte-exact=OK" not in stable_repeat_output:
            raise RuntimeError(f"complete-program repeat was not exact:\n{stable_repeat_output}")
        if stable_repeat_wire.read_bytes() != stable_wire.read_bytes():
            raise RuntimeError("complete-program literal replay differs")
        if stable_repeat_curve.read_bytes() != stable_curve.read_bytes():
            raise RuntimeError("complete-program curve replay differs")
        if stable_repeat_components.read_bytes() != stable_components.read_bytes():
            raise RuntimeError("complete-program component replay differs")

        prefix_manifest = root / "manifest-first.txt"
        prefix_manifest.write_text("".join(f"{path}\n" for path in paths[:3]))
        prefix_plan = root / "p29-first"
        prefix_common = common.copy()
        prefix_common[1] = str(prefix_manifest)
        prefix_baseline = run(
            [str(legacy), *prefix_common, "--mixed-dump-prefix", str(prefix_plan)]
        )
        if "byte-exact=OK" not in prefix_baseline:
            raise RuntimeError(f"prefix plan was not exact:\n{prefix_baseline}")

        prefix_wire = root / "stable-prefix.wire"
        prefix_curve = root / "stable-prefix.tsv"
        prefix_components = root / "stable-prefix-components.tsv"
        prefix_output = run(
            [
                str(grouped),
                *prefix_common,
                "--literal-group-prefix",
                str(prefix_plan),
                "--literal-group-tus",
                "3",
                "--stable-root-tags",
                "--open-final-entropy",
                "--literal-group-skip-zstd10",
                "--literal-group-wire",
                str(prefix_wire),
                "--curve-tsv",
                str(prefix_curve),
                "--component-curve-tsv",
                str(prefix_components),
            ]
        )
        if "byte-exact=OK" not in prefix_output:
            raise RuntimeError(f"standalone prefix run was not exact:\n{prefix_output}")

        stable_curve_rows = list(csv.DictReader(stable_curve.open(), delimiter="\t"))
        prefix_curve_rows = list(csv.DictReader(prefix_curve.open(), delimiter="\t"))
        stable_component_rows = list(
            csv.DictReader(stable_components.open(), delimiter="\t")
        )
        prefix_component_rows = list(
            csv.DictReader(prefix_components.open(), delimiter="\t")
        )
        if stable_curve_rows[:3] != prefix_curve_rows:
            raise RuntimeError("full-run and standalone first-group curves differ")
        if stable_component_rows[:3] != prefix_component_rows:
            raise RuntimeError("full-run and standalone first-group component ledgers differ")
        if sum(int(row["block_wire_bytes"]) for row in prefix_component_rows) == 0:
            raise RuntimeError("prefix identity fixture did not exercise a Block definition")

        complete_wire = stable_wire.read_bytes()
        standalone_wire = prefix_wire.read_bytes()
        first_header = struct.unpack_from("<I", complete_wire)[0]
        first_frame_size = 4 + (first_header & ((1 << 29) - 1))
        if complete_wire[:first_frame_size] != standalone_wire:
            raise RuntimeError("full-run and standalone first literal-group frames differ")

        second_manifest = root / "manifest-first-two-groups.txt"
        second_manifest.write_text("".join(f"{path}\n" for path in paths[:6]))
        second_plan = root / "p29-first-two-groups"
        second_common = common.copy()
        second_common[1] = str(second_manifest)
        if "byte-exact=OK" not in run(
            [str(legacy), *second_common, "--mixed-dump-prefix", str(second_plan)]
        ):
            raise RuntimeError("two-group prefix plan was not exact")
        second_wire = root / "stable-prefix-two-groups.wire"
        second_curve = root / "stable-prefix-two-groups.tsv"
        second_components = root / "stable-prefix-two-groups-components.tsv"
        second_output = run(
            [
                str(grouped),
                *second_common,
                "--literal-group-prefix",
                str(second_plan),
                "--literal-group-tus",
                "3",
                "--stable-root-tags",
                "--open-final-entropy",
                "--literal-group-skip-zstd10",
                "--literal-group-wire",
                str(second_wire),
                "--curve-tsv",
                str(second_curve),
                "--component-curve-tsv",
                str(second_components),
            ]
        )
        if "byte-exact=OK" not in second_output:
            raise RuntimeError(f"two-group standalone prefix was not exact:\n{second_output}")
        second_curve_rows = list(csv.DictReader(second_curve.open(), delimiter="\t"))
        second_component_rows = list(
            csv.DictReader(second_components.open(), delimiter="\t")
        )
        if stable_curve_rows[:6] != second_curve_rows:
            raise RuntimeError("full-run and standalone two-group curves differ")
        if stable_component_rows[:6] != second_component_rows:
            raise RuntimeError("full-run and standalone two-group component ledgers differ")
        second_frame_offset = first_frame_size
        second_header = struct.unpack_from("<I", complete_wire, second_frame_offset)[0]
        second_frame_end = (
            second_frame_offset + 4 + (second_header & ((1 << 29) - 1))
        )
        if complete_wire[:second_frame_end] != second_wire.read_bytes():
            raise RuntimeError("full-run and standalone first two literal-group frames differ")

        incomplete_without_stable_ids = run(
            [
                str(grouped),
                *common,
                "--literal-group-prefix",
                str(prefix),
                "--literal-group-tus",
                "3",
                "--open-final-entropy",
            ],
            expect_success=False,
        )
        if "--open-final-entropy requires --stable-root-tags" not in incomplete_without_stable_ids:
            raise RuntimeError(
                "unbound open-prefix mode failed for the wrong reason:\n"
                f"{incomplete_without_stable_ids}"
            )

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

    print(
        "P29 bounded literal groups exactness/accounting/rejection and prefix identity PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
