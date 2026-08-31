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
from farmharness.s4_version_transition_planner import build_plan


class S4MethodMatrixExecutorTest(unittest.TestCase):
    def test_materializes_exact_planner_grid_for_one_calibration_corpus(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            campaign = materialize_matrix(
                output_root=Path(temp), repo=Path.cwd(), corpus="fmt",
                source_manifest="/inputs/{corpus}/{profile}/{regime}/{topology}.json",
                source_root="/inputs", matrix_audit=Path("/audit.json"),
                engine_manifest="/engine/{profile}.json", product_root=Path("/product"),
                compile_db=Path("/compile_commands.json"),
                compile_source_root=Path("/compile-src"),
                timestamp="20260831T141130Z")
            summary = json.loads((campaign / "summary.json").read_text())
            self.assertEqual(summary["status"], "STAGED")
            self.assertFalse(summary["execution_ready"])
            self.assertEqual(summary["comparison_blocks"], 128)
            self.assertEqual(summary["arm_runs"], 256)
            self.assertEqual(summary["blocked_arms"], 0)
            self.assertEqual(summary["staged_arms"], 256)
            self.assertIsNone(summary["raw_ii_gap"])
            self.assertEqual(summary["split_policy"]["campaign_corpus"], "fmt")
            self.assertFalse(any("DuckDB" in str(path) or "LLVM-1238" in str(path)
                                 for path in campaign.rglob("*")))
            expected_ids = {row["id"] for row in build_plan()["execution_measurement_contract"]["measurement_cells"]}
            observed_ids = {json.loads(path.read_text())["id"]
                            for path in campaign.glob("experiments/*/*/*/*/*/*/manifest.json")}
            self.assertEqual(observed_ids, expected_ids)

            grz = next(campaign.glob("experiments/fmt/GRZ/full/*/cold/AB"))
            method = json.loads((grz / "arms/GRZ/manifest.json").read_text())
            self.assertEqual(method["product_profile"], "GRZ")
            self.assertEqual(method["harness_profile"], "GRZ_RESIDUAL")
            self.assertTrue(method["executable"])
            self.assertNotIn("execution_blocker", method)
            self.assertTrue(all(row["executable"] for row in method["commands"]))
            self.assertEqual(method["measurement"]["channel_bytes"], None)
            live = next(row for row in method["commands"] if row["stage"] == "live_run")
            self.assertIn("--profile", live["argv"])
            self.assertEqual(live["argv"][live["argv"].index("--profile") + 1], "GRZ_RESIDUAL")

            repeat = campaign / "experiments" / "fmt" / "GRZ" / "repeat-full" / "C1F1-100000" / "cold" / "AB"
            repeat_method = json.loads((repeat / "arms/GRZ/manifest.json").read_text())
            producer = next(row for row in repeat_method["commands"]
                            if row["stage"] == "predictive_producer")
            self.assertIn("--repeat-plan", producer["argv"])
            repeat_live = next(row for row in repeat_method["commands"]
                               if row["stage"] == "live_run")
            self.assertIn("--repeat-predictive-plan", repeat_live["argv"])
            self.assertEqual(sum(row["stage"].startswith("comparison")
                                 for row in repeat_method["commands"]), 2)
            predecessor = Path(repeat_method["repeat_predecessor"]["path"])
            expected_predecessor = (campaign / "experiments" / "fmt" / "GRZ" / "full" /
                                    "C1F1-100000" / "cold" / "AB" / "arms" / "GRZ" /
                                    "attempt-001" / "depth-plan.json")
            self.assertEqual(predecessor, expected_predecessor)
            self.assertEqual(repeat_method["repeat_predecessor"]["corpus"], "fmt")
            self.assertEqual(repeat_method["repeat_predecessor"]["method"], "GRZ")
            self.assertEqual(repeat_method["repeat_predecessor"]["topology"], "C1F1/100000")
            self.assertEqual(repeat_method["repeat_predecessor"]["regime"], "cold")
            self.assertEqual(repeat_method["repeat_predecessor"]["order"], "AB")

            raw = json.loads((grz / "arms/RAW_II/manifest.json").read_text())
            self.assertEqual(raw["status"], "STAGED")
            self.assertTrue(raw["executable"])
            self.assertEqual(raw["harness_profile"], "ZSTD_TU")
            raw_live = next(row for row in raw["commands"] if row["stage"] == "live_run")
            self.assertEqual(raw_live["argv"][raw_live["argv"].index("--profile") + 1], "ZSTD_TU")
            self.assertEqual(raw_live["argv"][raw_live["argv"].index("--product-profile") + 1], "RAW_II")

    def test_execute_fails_closed_without_running_anything(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "intentionally unavailable"):
                materialize_matrix(
                    output_root=Path(temp), repo=Path.cwd(), corpus="fmt", source_manifest="x",
                    source_root="x", matrix_audit=Path("x"), engine_manifest="x",
                    product_root=Path("x"), execute=True)

    def test_heldout_corpus_is_rejected_before_materialization(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "held-out"):
                materialize_matrix(
                    output_root=Path(temp), repo=Path.cwd(), corpus="DuckDB",
                    source_manifest="x", source_root="x", matrix_audit=Path("x"),
                    engine_manifest="x", product_root=Path("x"))


if __name__ == "__main__":
    unittest.main()
