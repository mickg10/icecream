#!/usr/bin/env python3
"""Focused tests for the non-executing S4-to-S8 command materializer."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from farmharness.s4_method_matrix_executor import (
    RAW_II_GAP,
    materialize_matrix,
)


class S4MethodMatrixExecutorTest(unittest.TestCase):
    def test_materializes_both_calibration_corpora_and_keeps_raw_gap_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            campaign = materialize_matrix(
                output_root=Path(temp), repo=Path.cwd(),
                source_manifest="/inputs/{corpus}/{profile}/{regime}/{topology}.json",
                source_root="/inputs", matrix_audit=Path("/audit.json"),
                engine_manifest="/engine/{profile}.json", product_root=Path("/product"),
                compile_db=Path("/compile_commands.json"),
                compile_source_root=Path("/compile-src"),
                timestamp="20260831T141130Z")
            summary = json.loads((campaign / "summary.json").read_text())
            self.assertEqual(summary["status"], "STAGED_RAW_II_GAP")
            self.assertEqual(summary["comparison_blocks"], 256)
            self.assertEqual(summary["arm_runs"], 512)
            self.assertEqual(summary["blocked_arms"], 256)
            self.assertEqual(summary["staged_arms"], 256)
            self.assertEqual(summary["raw_ii_gap"], RAW_II_GAP)
            self.assertFalse(any("DuckDB" in str(path) or "LLVM-1238" in str(path)
                                 for path in campaign.rglob("*")))

            grz = next(campaign.glob("experiments/fmt/GRZ/full/*/cold/AB"))
            method = json.loads((grz / "arms/GRZ/manifest.json").read_text())
            self.assertEqual(method["product_profile"], "GRZ")
            self.assertEqual(method["harness_profile"], "GRZ_RESIDUAL")
            self.assertTrue(method["executable"])
            self.assertEqual(method["measurement"]["channel_bytes"], None)
            live = next(row for row in method["commands"] if row["stage"] == "live_run")
            self.assertIn("--profile", live["argv"])
            self.assertEqual(live["argv"][live["argv"].index("--profile") + 1], "GRZ_RESIDUAL")

            repeat = next(campaign.glob("experiments/fmt/GRZ/repeat-full/*/cold/AB"))
            repeat_method = json.loads((repeat / "arms/GRZ/manifest.json").read_text())
            producer = next(row for row in repeat_method["commands"]
                            if row["stage"] == "predictive_producer")
            self.assertIn("--repeat-plan", producer["argv"])
            repeat_live = next(row for row in repeat_method["commands"]
                               if row["stage"] == "live_run")
            self.assertIn("--repeat-predictive-plan", repeat_live["argv"])
            self.assertEqual(sum(row["stage"].startswith("comparison")
                                 for row in repeat_method["commands"]), 2)

            raw = json.loads((grz / "arms/RAW_II/manifest.json").read_text())
            self.assertEqual(raw["status"], "BLOCKED")
            self.assertFalse(raw["executable"])
            self.assertEqual(raw["commands"], [])
            self.assertIn("does not provide the required multi-TU transfer curve", raw["gap"])

    def test_execute_fails_closed_without_running_anything(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "RAW_II bridge"):
                materialize_matrix(
                    output_root=Path(temp), repo=Path.cwd(), source_manifest="x",
                    source_root="x", matrix_audit=Path("x"), engine_manifest="x",
                    product_root=Path("x"), execute=True)


if __name__ == "__main__":
    unittest.main()
