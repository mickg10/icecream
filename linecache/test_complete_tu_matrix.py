#!/usr/bin/env python3
"""Focused checks for the complete-codec project/schema matrix."""

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

from run_complete_tu_matrix import SCHEMAS, schema_rows, validate_curve
from summarize_complete_tu_matrix import checkpoint_point


class CompleteTuMatrixTest(unittest.TestCase):
    def test_schema_ladder_has_all_receiver_states(self) -> None:
        rows = schema_rows()
        self.assertEqual(len(rows), 15)
        self.assertEqual(
            {(row["stage"], row["state"]) for row in rows},
            {
                (f"p{stage}", state)
                for stage in range(25, 30)
                for state in ("cold", "bit0", "bit1")
            },
        )
        for row in rows:
            flags = row["flags"]
            if row["stage"] == "p25":
                self.assertEqual("--direct-ordinals" in flags, False)
            else:
                self.assertEqual("--direct-ordinals" in flags, True)
            if row["state"] == "cold":
                self.assertEqual("--half-cold-bit" in flags, False)
            else:
                position = flags.index("--half-cold-bit")
                self.assertEqual(flags[position + 1], row["state"][-1])

    def test_curve_validation_and_checkpoint_accounting(self) -> None:
        rows = [
            {
                "tu": "1",
                "raw_bytes": "1000",
                "wire_bytes": "100",
                "cumulative_raw_bytes": "1000",
                "cumulative_wire_bytes": "100",
                "cumulative_ratio": "10",
                "exact": "true",
            },
            {
                "tu": "2",
                "raw_bytes": "3000",
                "wire_bytes": "150",
                "cumulative_raw_bytes": "4000",
                "cumulative_wire_bytes": "250",
                "cumulative_ratio": "16",
                "exact": "true",
            },
        ]
        with tempfile.TemporaryDirectory() as directory:
            curve = Path(directory) / "curve.tsv"
            with curve.open("w", newline="") as output:
                writer = csv.DictWriter(
                    output,
                    fieldnames=tuple(rows[0]),
                    delimiter="\t",
                    lineterminator="\n",
                )
                writer.writeheader()
                writer.writerows(rows)
            log = (
                "loaded+interned 0.1s TUs=2 raw=4000 regions=1\n"
                "byte-exact=OK  TUs=2 raw=0.0 MiB TOTAL=250\n"
            )
            self.assertEqual(
                validate_curve(curve, log, 2),
                {"tus": 2, "raw_bytes": 4000, "wire_bytes": 250},
            )
        point = checkpoint_point(rows, 2)
        assert point is not None
        self.assertEqual(point["wire_bytes"], 250)
        self.assertEqual(point["decimal_mb"], 0.00025)
        self.assertEqual(point["seconds_1gbit"], 0.000002)
        self.assertIsNone(checkpoint_point(rows, 3))

    def test_exported_schemas_are_stable(self) -> None:
        self.assertEqual(SCHEMAS, schema_rows())


if __name__ == "__main__":
    unittest.main()
