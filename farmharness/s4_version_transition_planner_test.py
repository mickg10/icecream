#!/usr/bin/env python3
"""Tests for the pure, non-executing S4 version transition planner."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

from farmharness.s4_version_transition_planner import (
    P43_SOURCE_SHA,
    P44_PROTOCOL_ASSERTION,
    P44_SOURCE_SHA,
    P50_SOURCE_SHA,
    REAL_VERSIONS,
    STATIC_STATES,
    audit_plan,
    build_plan,
    state_id,
)


class S4VersionTransitionPlannerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.plan = build_plan()

    def test_exact_artifact_authorities_are_distinct(self) -> None:
        rows = {row["version"]: row for row in self.plan["real_artifact_versions"]}
        self.assertEqual(rows[43]["source_commit"], P43_SOURCE_SHA)
        self.assertEqual(rows[44]["source_commit"], P44_SOURCE_SHA)
        self.assertEqual(rows[50]["source_commit"], P50_SOURCE_SHA)
        self.assertEqual(rows[44]["protocol_assertion"]["text"], P44_PROTOCOL_ASSERTION)
        self.assertNotEqual(rows[43]["source_commit"], rows[44]["source_commit"])

    def test_static_grid_and_ordered_pair_grid(self) -> None:
        self.assertEqual(len(STATIC_STATES), 27)
        self.assertEqual(self.plan["state_count"], 27)
        self.assertEqual(self.plan["transition_count"], 729)
        self.assertEqual(self.plan["transition_class_counts"],
                         {"no-op": 27, "one-role": 162, "multi-role": 540})
        self.assertEqual(audit_plan(self.plan)["status"], "PASS")

    def test_only_all_current_roles_preregister_p50_cache(self) -> None:
        for state in self.plan["states"]:
            expected = state["id"] == "s50-c50-f50"
            self.assertIs(state["cache_expected"], expected)
            self.assertEqual(state["remote_output"], "byte-exact-required")
            if not expected:
                self.assertEqual(state["p50_cache_traffic"], "zero-required")
        p44 = next(row for row in self.plan["states"] if row["id"] == "s44-c44-f44")
        self.assertEqual(p44["old_p44_cache_behavior"], "UNRESOLVED")

    def test_homogeneous_performance_arms_include_p50_legacy_and_current(self) -> None:
        arms = self.plan["performance_arms"]
        self.assertEqual(self.plan["homogeneous_states"],
                         ["s43-c43-f43", "s44-c44-f44", "s50-c50-f50"])
        self.assertEqual(arms["p43_homogeneous_legacy"]["state"], "s43-c43-f43")
        self.assertEqual(arms["p44_homogeneous_legacy"]["state"], "s44-c44-f44")
        self.assertTrue(arms["p50_raw_ii_whole_legacy"]["cache_disabled"])
        self.assertEqual(self.plan["homogeneous_version_arms"], {
            "P43": "p43_homogeneous_legacy",
            "P44": "p44_homogeneous_legacy",
            "P50": "p50_raw_ii_whole_legacy",
        })

    def test_p50_methods_are_named_and_cover_every_execution_dimension(self) -> None:
        contract = self.plan["execution_measurement_contract"]
        self.assertEqual(contract["methods"], ["ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ"])
        self.assertEqual(contract["depths"], ["100", "200", "full", "repeat-full"])
        self.assertEqual([item["id"] for item in contract["topologies"]],
                         ["C1F1/100000", "C1F20/40"])
        self.assertEqual(contract["regimes"], ["cold", "warm"])
        self.assertEqual(contract["orders"], ["AB", "BA"])
        self.assertTrue(contract["counterbalanced"])
        self.assertEqual(len(contract["measurement_cells"]), 128)
        self.assertEqual(set(arms for arms in self.plan["performance_arms"] if arms.startswith("p50_")),
                         {"p50_raw_ii_whole_legacy", "p50_zstd_tu", "p50_zstd_route", "p50_p29", "p50_grz"})
        self.assertTrue(all(item["state"] == "s50-c50-f50" and
                            item["byte_identical_required"]
                            for item in contract["measurement_cells"]))

    def test_each_baseline_has_six_upgrade_and_downgrade_orders(self) -> None:
        self.assertEqual({key: len(value) for key, value in self.plan["upgrade_orders"].items()},
                         {"43_to_50": 6, "44_to_50": 6})
        self.assertEqual({key: len(value) for key, value in self.plan["downgrade_orders"].items()},
                         {"50_to_43": 6, "50_to_44": 6})
        for order in self.plan["upgrade_orders"]["43_to_50"]:
            self.assertEqual(len(order["states"]), 4)
            self.assertEqual(order["states"][0], "s43-c43-f43")
            self.assertEqual(order["states"][-1], "s50-c50-f50")

    def test_p48_p49_are_extras_only(self) -> None:
        self.assertEqual([row["version"] for row in self.plan["boundary_extras"]], [48, 49])
        self.assertTrue(all(not row["required_real_artifact"] for row in self.plan["boundary_extras"]))
        self.assertFalse(any(48 in state or 49 in state for state in STATIC_STATES))

    def test_cohort_and_global_are_optional_unaccepted_extras(self) -> None:
        extras = self.plan["optional_unaccepted_method_extras"]
        for method in ("ZSTD_COHORT", "ZSTD_GLOBAL"):
            self.assertFalse(extras[method]["implemented"])
            self.assertFalse(extras[method]["accepted"])
            self.assertFalse(extras[method]["required"])

    def test_auditor_is_fail_closed_on_cache_mutation(self) -> None:
        mutant = json.loads(json.dumps(self.plan))
        row = next(item for item in mutant["states"] if item["id"] == "s50-c50-f50")
        row["cache_expected"] = False
        self.assertEqual(audit_plan(mutant)["status"], "FAIL")

    def test_cli_is_non_executing_and_emits_json(self) -> None:
        script = Path(__file__).with_name("s4_version_transition_planner.py")
        completed = subprocess.run([sys.executable, str(script), "--summary"],
                                   check=True, capture_output=True, text=True)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(summary["state_count"], 27)
        self.assertEqual(summary["transition_count"], 729)


if __name__ == "__main__":
    unittest.main()
