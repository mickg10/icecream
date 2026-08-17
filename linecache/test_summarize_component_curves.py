#!/usr/bin/env python3
"""Tests for the exact component-curve summarizer."""

from __future__ import annotations

import csv
import json
import tempfile
import unittest
from pathlib import Path

import summarize_component_curves as summary


class ComponentCurveTest(unittest.TestCase):
    def write_curve(self, path: Path, bad_total: bool = False) -> None:
        columns = (
            "tu",
            "raw_bytes",
            "wire_bytes",
            *summary.RAW_COLUMNS,
            *summary.WIRE_COLUMNS,
            "cumulative_raw_bytes",
            "cumulative_wire_bytes",
            "exact",
        )
        with path.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=columns, delimiter="\t")
            writer.writeheader()
            cumulative_raw = cumulative_wire = 0
            for tu, raw_bytes, root_bytes, literal_bytes in (
                (1, 1000, 10, 90),
                (2, 2000, 20, 180),
            ):
                wire_bytes = root_bytes + literal_bytes
                cumulative_raw += raw_bytes
                cumulative_wire += wire_bytes
                row = {column: 0 for column in (*summary.RAW_COLUMNS, *summary.WIRE_COLUMNS)}
                row.update(
                    {
                        "tu": tu,
                        "raw_bytes": raw_bytes,
                        "wire_bytes": wire_bytes + (1 if bad_total and tu == 2 else 0),
                        "literal_raw_bytes": raw_bytes,
                        "root_wire_bytes": root_bytes,
                        "literal_wire_bytes": literal_bytes,
                        "cumulative_raw_bytes": cumulative_raw,
                        "cumulative_wire_bytes": cumulative_wire
                        + (1 if bad_total and tu == 2 else 0),
                        "exact": "true",
                    }
                )
                writer.writerow(row)

    def test_exact_summary(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            curve = Path(raw_directory) / "curve.tsv"
            self.write_curve(curve)
            rows = summary.read_curve("test", curve)
            result = summary.summarize_corpus("test", curve, rows)
            self.assertEqual(result["raw_bytes"], 3000)
            self.assertEqual(result["wire_bytes"], 300)
            self.assertEqual(result["family_wire_bytes"]["literal"], 270)
            self.assertEqual(result["family_wire_bytes"]["structure"], 30)
            self.assertEqual(result["per_tu_wire_distribution"]["max_bytes"], 200)

    def test_component_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            curve = Path(raw_directory) / "curve.tsv"
            self.write_curve(curve, bad_total=True)
            with self.assertRaisesRegex(ValueError, "components=200, wire=201"):
                summary.read_curve("test", curve)

    def test_precomputed_projection_reconciles_to_curve(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            root = Path(raw_directory)
            curve = root / "curve.tsv"
            evidence = root / "precomputed.json"
            self.write_curve(curve)
            corpus = summary.summarize_corpus(
                "test", curve, summary.read_curve("test", curve)
            )
            evidence.write_text(
                json.dumps(
                    {
                        "schema": "precomputed-material-lane-ceiling-v1",
                        "corpora": [
                            {
                                "name": "test",
                                "tus": 2,
                                "whole_ii_z19_bytes": 250,
                                "cold_gate_bytes": 275,
                                "current_complete_wire_bytes": 300,
                                "fixed_wire_bytes": 30,
                                "lanes": [
                                    {
                                        "name": "literal",
                                        "raw_component": "literal",
                                        "wire_components": ["literal"],
                                        "raw_bytes": 3000,
                                        "current_wire_bytes": 270,
                                        "candidate_bytes": {
                                            "zstd19_long": 200,
                                            "zpaq_m3": 190,
                                            "zpaq_m4": 180,
                                            "zpaq_m5": 170,
                                        },
                                        "decoded_sha256_verified": True,
                                    }
                                ],
                            }
                        ],
                    }
                )
            )
            result = summary.load_precomputed(evidence, [corpus])
            projected = result["corpora"][0]["projections"]
            self.assertEqual(projected["zstd19_long"], 230)
            self.assertEqual(projected["zpaq_m5"], 200)
            self.assertEqual(projected["best_zstd19_or_m3_per_lane"], 220)


if __name__ == "__main__":
    unittest.main()
