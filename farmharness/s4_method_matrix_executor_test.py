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
            self.assertEqual(summary["status"], "NOT_READY")
            self.assertFalse(summary["execution_ready"])
            self.assertEqual(summary["comparison_blocks"], 128)
            self.assertEqual(summary["arm_runs"], 256)
            self.assertEqual(summary["compatibility_states"], 27)
            self.assertEqual(summary["transition_rows"], 729)
            compatibility = campaign / summary["compatibility_plan"]
            transitions = campaign / summary["transition_plan"]
            self.assertEqual(len(compatibility.read_text().splitlines()), 27)
            self.assertEqual(len(transitions.read_text().splitlines()), 729)
            first = json.loads(compatibility.read_text().splitlines()[0])
            self.assertEqual(first["schema"], "icecream-s4-compatibility-experiment-v1")
            self.assertEqual(first["timestamp"], "20260831T141130Z")
            self.assertEqual(first["id"], "s43-c43-f43")
            self.assertEqual(json.loads(transitions.read_text().splitlines()[0])["kind"], "transition")
            self.assertEqual(summary["blocked_arms"], 256)
            self.assertEqual(summary["staged_arms"], 0)
            self.assertEqual(summary["raw_ii_gap"],
                             "RAW_II witness and control-engine inputs are not bound")
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
            self.assertFalse(method["executable"])
            self.assertIn("execution_blocker", method)
            self.assertTrue(all(not row["executable"] for row in method["commands"]))
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
            self.assertEqual(raw["status"], "NOT_READY")
            self.assertFalse(raw["executable"])
            self.assertEqual(raw["harness_profile"], "RAW_II")
            self.assertEqual(raw["transfer_accounting"]["basis"],
                             "framed_application_wire_bytes")
            raw_live = next(row for row in raw["commands"] if row["stage"] == "live_run")
            self.assertEqual(raw_live["argv"][raw_live["argv"].index("--profile") + 1], "RAW_II")
            self.assertEqual(raw_live["argv"][raw_live["argv"].index("--product-profile") + 1], "RAW_II")
            raw_producer = next(row for row in raw["commands"]
                                if row["stage"] == "predictive_producer")
            self.assertTrue(raw_producer["argv"][1].endswith("s8_raw_ii_predictive_producer.py"))
            self.assertNotIn("s8_multitu_predictive_producer.py", raw_producer["argv"][1])
            self.assertNotIn("ZSTD_TU", raw_producer["argv"])
            self.assertNotIn("P29", raw_producer["argv"])

    def test_execute_fails_closed_without_running_anything(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "intentionally unavailable"):
                materialize_matrix(
                    output_root=Path(temp), repo=Path.cwd(), corpus="fmt", source_manifest="x",
                    source_root="x", matrix_audit=Path("x"), engine_manifest="x",
                    product_root=Path("x"), execute=True)

    def test_staged_commands_bind_to_their_materialized_plans_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            campaign = materialize_matrix(
                output_root=Path(temp), repo=Path.cwd(), corpus="fmt",
                source_manifest="/inputs/{corpus}/{profile}/{regime}/{topology}.json",
                source_root="/inputs", matrix_audit=Path("/audit.json"),
                engine_manifest="/engine/{profile}.json", product_root=Path("/product"),
                compile_db=Path("/compile_commands.json"),
                compile_source_root=Path("/compile-src"), timestamp="20260831T141131Z")
            for manifest_path in campaign.glob("experiments/*/*/*/*/*/*/arms/*/manifest.json"):
                manifest = json.loads(manifest_path.read_text())
                commands = {row["stage"]: row for row in manifest["commands"]}
                planner = commands["predictive_plan"]["argv"]
                producer = commands["predictive_producer"]["argv"]
                plan_path = Path(planner[planner.index("--out") + 1])
                self.assertEqual(plan_path, Path(producer[producer.index("--repeat-plan" if "--repeat-plan" in producer else "--plan") + 1]))
                self.assertEqual(producer.count("--repeat-plan"),
                                 1 if "repeat-full" in manifest_path.parts else 0)
                if "repeat-full" in manifest_path.parts:
                    self.assertNotIn("--output-dir", producer)
                self.assertFalse(commands["predictive_plan"]["executable"])
                self.assertFalse(commands["predictive_producer"]["executable"])
                if manifest["method"] == "RAW_II":
                    self.assertTrue(producer[1].endswith("s8_raw_ii_predictive_producer.py"))
                    self.assertNotIn("ZSTD_TU", producer)
                    self.assertNotIn("P29", producer)
                else:
                    self.assertTrue(producer[1].endswith("s8_multitu_predictive_producer.py"))

            compatibility = [json.loads(line) for line in
                             (campaign / "compatibility-plan.jsonl").read_text().splitlines()]
            self.assertEqual({row["id"] for row in compatibility},
                             {row["id"] for row in build_plan()["compatibility_matrix"]})
            transition = [json.loads(line) for line in
                          (campaign / "transition-plan.jsonl").read_text().splitlines()]
            self.assertEqual({(row["before"], row["after"]) for row in transition},
                             {(row["before"], row["after"]) for row in build_plan()["transitions"]})

    def test_matrix_audit_gate_requires_authenticated_complete_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source_root = root / "sources"
            source_root.mkdir()
            source_manifest = root / "sources.txt"
            source_manifest.write_text("placeholder\n")
            engine_manifest = root / "engine.json"
            engine_manifest.write_text("{}\n")
            matrix_audit = root / "matrix.json"
            matrix_audit.write_text(json.dumps({
                "schema": "icecream-s8-matrix-audit-v1", "status": "PASS",
                "matrix": {"expected_cells": 32, "completed_cells": 32,
                            "missing_cells": [], "invalid_candidates": [],
                            "calibration_cells": 16, "held_out_validation_cells": 16},
                "cells": [],
            }) + "\n")
            compile_db = root / "compile_commands.json"
            compile_db.write_text("[]\n")
            compile_source = root / "compile-src"
            compile_source.mkdir()
            product = root / "product"
            product.mkdir()

            campaign = materialize_matrix(
                output_root=root, repo=Path.cwd(), corpus="fmt",
                source_manifest=str(source_manifest), source_root=str(source_root),
                matrix_audit=matrix_audit, engine_manifest=str(engine_manifest),
                product_root=product, compile_db=compile_db,
                compile_source_root=compile_source, timestamp="20260831T141132Z")
            grz_path = campaign / "experiments/fmt/GRZ/full/C1F1-100000/cold/AB/arms/GRZ/manifest.json"
            grz = json.loads(grz_path.read_text())
            self.assertEqual(grz["status"], "STAGED")
            self.assertTrue(grz["executable"])

            matrix_audit.write_text(json.dumps({
                "schema": "icecream-s8-matrix-audit-v1", "status": "INCOMPLETE",
                "matrix": {"expected_cells": 32, "completed_cells": 31,
                            "missing_cells": ["missing"], "invalid_candidates": [],
                            "calibration_cells": 16, "held_out_validation_cells": 15},
                "cells": [],
            }) + "\n")
            blocked_campaign = materialize_matrix(
                output_root=root, repo=Path.cwd(), corpus="fmt",
                source_manifest=str(source_manifest), source_root=str(source_root),
                matrix_audit=matrix_audit, engine_manifest=str(engine_manifest),
                product_root=product, compile_db=compile_db,
                compile_source_root=compile_source, timestamp="20260831T141133Z")
            blocked = json.loads((blocked_campaign /
                                  "experiments/fmt/GRZ/full/C1F1-100000/cold/AB/arms/GRZ/manifest.json").read_text())
            self.assertEqual(blocked["status"], "BLOCKED")
            self.assertFalse(blocked["executable"])
            self.assertIn("matrix audit status is not PASS", blocked["execution_blocker"])

    def test_heldout_corpus_is_rejected_before_materialization(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "held-out"):
                materialize_matrix(
                    output_root=Path(temp), repo=Path.cwd(), corpus="DuckDB",
                    source_manifest="x", source_root="x", matrix_audit=Path("x"),
                    engine_manifest="x", product_root=Path("x"))


if __name__ == "__main__":
    unittest.main()
