#!/usr/bin/env python3

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

import summarize_native9_holdout as summary


def write_tsv(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


class NativeNineSummaryTest(unittest.TestCase):
    def test_p29_curve_requires_every_exact_cumulative_row(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            curve = Path(directory) / "curve.tsv"
            rows = [
                {
                    "tu": tu,
                    "raw_bytes": 10 * tu,
                    "wire_bytes": tu,
                    "cumulative_raw_bytes": sum(10 * prior for prior in range(1, tu + 1)),
                    "cumulative_wire_bytes": sum(range(1, tu + 1)),
                    "cumulative_ratio": "1",
                    "exact": "true",
                }
                for tu in range(1, 201)
            ]
            write_tsv(curve, rows)
            points = summary.verify_p29_curve(
                curve,
                200,
                int(rows[-1]["cumulative_raw_bytes"]),
                int(rows[-1]["cumulative_wire_bytes"]),
            )
            self.assertEqual(points[100], (50500, 5050))
            self.assertEqual(points[200], (201000, 20100))

            rows[70]["cumulative_wire_bytes"] = 0
            write_tsv(curve, rows)
            with self.assertRaisesRegex(ValueError, "cumulative ledger does not close"):
                summary.verify_p29_curve(curve, 200, 201000, 20100)

    def test_grz_curve_requires_physical_frame_closure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            curve = Path(directory) / "curve.tsv"
            rows = [
                {
                    "group": 0,
                    "tu_lo": 0,
                    "tu_hi": 100,
                    "out_bytes": 1000,
                    "add_bytes": 50,
                    "comp_bytes": 100,
                    "ratio": "10",
                    "hist_base": 0,
                    "hist_extent": 0,
                    "anchor_samples": 0,
                    "anchor_occupied": 0,
                    "anchor_usable": 0,
                    "anchor_collisions": 0,
                    "anchor_matches": 0,
                    "closed_by": "tu",
                },
                {
                    "group": 1,
                    "tu_lo": 100,
                    "tu_hi": 200,
                    "out_bytes": 2000,
                    "add_bytes": 50,
                    "comp_bytes": 200,
                    "ratio": "10",
                    "hist_base": 0,
                    "hist_extent": 1000,
                    "anchor_samples": 0,
                    "anchor_occupied": 0,
                    "anchor_usable": 0,
                    "anchor_collisions": 0,
                    "anchor_matches": 0,
                    "closed_by": "tu",
                },
            ]
            write_tsv(curve, rows)
            summary.verify_curve(curve, 200, 3000, 300 + summary.STREAM_OVERHEAD)
            with self.assertRaisesRegex(ValueError, "physical stream does not close"):
                summary.verify_curve(curve, 200, 3000, 409)

    def test_frozen_selector_rule(self) -> None:
        policy = {
            "high_raw_bytes": 500_000_000,
            "low_raw_bytes": 200_000_000,
            "max_p29_trajectory": 0.3,
            "min_p29_literal_wire_fraction": 0.57,
            "max_p29_root_missing_wire_fraction": 0.05,
        }
        row = {
            "id": "large",
            "probe_raw_bytes": "500000000",
            "probe_wire_bytes": "1000",
            "probe_p29_trajectory": "0.3",
            "probe_literal_wire_bytes": "0",
            "probe_root_wire_bytes": "1000",
            "probe_missing_request_wire_bytes": "0",
        }
        self.assertTrue(summary.select_p29(row, policy)[0])
        row["probe_p29_trajectory"] = "0.3000001"
        self.assertFalse(summary.select_p29(row, policy)[0])


if __name__ == "__main__":
    unittest.main()
