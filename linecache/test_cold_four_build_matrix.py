#!/usr/bin/env python3
"""Focused checks for the cold retained-state four-build matrix."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from run_cold_four_build_matrix import (
    BUILD_REPETITIONS,
    COLD_SCHEMAS,
    canonical_manifest_entries,
    write_repeated_manifest,
)
from summarize_cold_four_build_matrix import (
    NO_SHARED_S_TRANSFER_BYTES,
    aggregate_by_stage,
    curve_point,
    format_mb_time,
    format_s_transfer,
    validate_repeated_curve,
)


def synthetic_curve(tus_per_build: int = 2) -> list[dict]:
    raw_pattern = (1_000, 3_000)
    wire_pattern = (100, 150, 20, 30, 10, 15, 8, 12)
    rows = []
    cumulative_raw = cumulative_wire = 0
    for ordinal in range(1, BUILD_REPETITIONS * tus_per_build + 1):
        raw = raw_pattern[(ordinal - 1) % tus_per_build]
        wire = wire_pattern[ordinal - 1]
        cumulative_raw += raw
        cumulative_wire += wire
        rows.append(
            {
                "tu": str(ordinal),
                "raw_bytes": str(raw),
                "wire_bytes": str(wire),
                "cumulative_raw_bytes": str(cumulative_raw),
                "cumulative_wire_bytes": str(cumulative_wire),
                "cumulative_ratio": str(cumulative_raw / cumulative_wire),
                "exact": "true",
            }
        )
    return rows


class ColdFourBuildMatrixTest(unittest.TestCase):
    def test_no_shared_s_transfer_is_explicitly_zero(self) -> None:
        self.assertEqual(NO_SHARED_S_TRANSFER_BYTES, 0)
        self.assertEqual(format_s_transfer("none", 0), "none · 0 B")
        self.assertEqual(
            format_s_transfer("example", 80_000), "0.1 MB · 0.001 s once"
        )

    def test_only_five_cold_schemas_are_selected(self) -> None:
        self.assertEqual(len(COLD_SCHEMAS), 5)
        self.assertEqual(
            [row["schema"] for row in COLD_SCHEMAS],
            [f"p{stage}-cold" for stage in range(25, 30)],
        )
        self.assertTrue(all(row["state"] == "cold" for row in COLD_SCHEMAS))

    def test_repeated_manifest_is_absolute_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_dir = root / "source"
            source_dir.mkdir()
            (source_dir / "a.ii").write_text("a")
            (source_dir / "b.ii").write_text("b")
            source = source_dir / "manifest.txt"
            source.write_text("a.ii\n\nb.ii\n")
            destination = root / "repeated" / "manifest.txt"
            metadata = write_repeated_manifest(source, destination)
            entries = canonical_manifest_entries(source)
            self.assertEqual(metadata["tus_per_build"], 2)
            self.assertEqual(metadata["total_tus"], 8)
            self.assertEqual(
                destination.read_text().splitlines(),
                list(entries) * BUILD_REPETITIONS,
            )
            first_hash = metadata["repeated_manifest_sha256"]
            self.assertEqual(
                write_repeated_manifest(source, destination)["repeated_manifest_sha256"],
                first_hash,
            )

    def test_curve_requires_four_identical_raw_builds(self) -> None:
        curve = synthetic_curve()
        self.assertEqual(validate_repeated_curve(curve), 2)
        self.assertEqual(curve_point(curve, 2)["wire_bytes"], 250)
        self.assertEqual(curve_point(curve, 8)["wire_bytes"], 345)
        changed = [dict(row) for row in curve]
        changed[4]["raw_bytes"] = "999"
        with self.assertRaisesRegex(ValueError, "raw TU sequence differs"):
            validate_repeated_curve(changed)

    def test_human_precision_is_unambiguous(self) -> None:
        self.assertEqual(format_mb_time(12_400_000), "12 MB · 0.099 s")
        self.assertEqual(format_mb_time(9_440_000), "9.4 MB · 0.076 s")

    def test_aggregate_keeps_incremental_builds(self) -> None:
        rows = []
        for schema in COLD_SCHEMAS:
            rows.append(
                {
                    "schema": schema["schema"],
                    "raw_bytes_per_build": 4_000,
                    "build1_incremental_wire_bytes": 250,
                    "build2_incremental_wire_bytes": 50,
                    "build3_incremental_wire_bytes": 25,
                    "build4_incremental_wire_bytes": 20,
                }
            )
        aggregates = aggregate_by_stage(rows)
        self.assertEqual(len(aggregates), 5)
        self.assertEqual(aggregates[0]["build_incremental_wire_bytes"], [250, 50, 25, 20])
        self.assertEqual(aggregates[0]["four_build_cumulative_wire_bytes"], 345)
        self.assertAlmostEqual(aggregates[0]["first_build_ratio"], 16.0)
        self.assertAlmostEqual(aggregates[0]["four_build_cumulative_ratio"], 16_000 / 345)


if __name__ == "__main__":
    unittest.main()
