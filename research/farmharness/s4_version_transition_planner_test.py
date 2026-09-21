#!/usr/bin/env python3
"""Tests for the pure, non-executing S4 version transition planner."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from research.farmharness.s4_version_transition_planner import (
    P43_SOURCE_SHA,
    P44_PROTOCOL_ASSERTION,
    P44_SOURCE_SHA,
    P50_SOURCE_SHA,
    REAL_VERSIONS,
    STATIC_STATES,
    audit_plan,
    build_plan,
    compatibility_for_state,
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

    def test_protocol_history_proves_both_legacy_comparators(self) -> None:
        self.assertEqual(self.plan["legacy_comparator"]["status"], "BOTH_P43_AND_P44")
        self.assertEqual(self.plan["legacy_comparator"]["versions"], [43, 44])
        history = {row["version"]: row for row in self.plan["protocol_history"]}
        self.assertEqual(history[43]["source_commit"], P43_SOURCE_SHA)
        self.assertEqual(history[44]["source_commit"], P44_SOURCE_SHA)
        self.assertEqual(history[44]["assertion"], P44_PROTOCOL_ASSERTION)
        self.assertEqual(history[50]["assertion"], "#define PROTOCOL_VERSION 50")

    def test_compatibility_matrix_has_pairwise_negotiation_and_distinct_arms(self) -> None:
        matrix = {row["id"]: row for row in self.plan["compatibility_matrix"]}
        self.assertEqual(len(matrix), 27)
        legacy = matrix["s43-c50-f44"]
        self.assertEqual(legacy["negotiated_protocols"], {"S-C": 43, "S-F": 43, "C-F": 44})
        self.assertEqual(legacy["expected_behavior"], "LEGACY_FALLBACK")
        self.assertFalse(legacy["feature_negotiation"]["p50_methods"])
        for method in ("RAW_II", "ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ"):
            self.assertEqual(legacy["method_arms"][method]["availability"], "unavailable")
            self.assertEqual(legacy["method_arms"][method]["fallback"], "WHOLE_LEGACY")
        current = matrix["s50-c50-f50"]
        self.assertEqual(current["expected_behavior"], "P50_METHODS")
        self.assertTrue(all(current["method_arms"][method]["availability"] == "supported"
                            for method in ("RAW_II", "ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ")))
        self.assertEqual(current["artifact_identities"]["S"]["runtime_source_commit"],
                         self.plan["source"]["harness"]["runner_p50_build_source_sha"])

    def test_auditor_rejects_compatibility_or_identity_deletion(self) -> None:
        mutant = json.loads(json.dumps(self.plan))
        del mutant["compatibility_matrix"][0]["method_arms"]["P29"]
        self.assertEqual(audit_plan(mutant)["status"], "FAIL")
        mutant = json.loads(json.dumps(self.plan))
        del mutant["states"][0]["compatibility"]
        self.assertEqual(audit_plan(mutant)["status"], "FAIL")
        mutant = json.loads(json.dumps(self.plan))
        mutant["compatibility_matrix"][0]["artifact_identities"]["S"]["version"] = 44
        self.assertEqual(audit_plan(mutant)["status"], "FAIL")

    def test_unsupported_protocol_triple_fails_closed(self) -> None:
        with self.assertRaises(ValueError):
            compatibility_for_state((43, 45, 50))

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
        self.assertEqual(contract["comparison_block_count"], 128)
        self.assertEqual(contract["execution_run_count"], 256)
        self.assertEqual(set(arms for arms in self.plan["performance_arms"] if arms.startswith("p50_")),
                         {"p50_raw_ii_whole_legacy", "p50_zstd_tu", "p50_zstd_route", "p50_p29", "p50_grz"})
        self.assertTrue(all(item["state"] == "s50-c50-f50" and
                            item["byte_identical_required"]
                            for item in contract["measurement_cells"]))
        block = contract["measurement_cells"][0]
        self.assertEqual(block["order"], "AB")
        self.assertEqual([arm["method"] for arm in block["sequence"]], ["RAW_II", block["method"]])
        self.assertNotIn("AB", block)
        self.assertNotIn("BA", block)
        ba_block = next(item for item in contract["measurement_cells"] if item["order"] == "BA")
        self.assertEqual([arm["method"] for arm in ba_block["sequence"]], [ba_block["method"], "RAW_II"])

    def test_version_cost_blocks_are_explicit_and_cache_free(self) -> None:
        contract = self.plan["homogeneous_version_comparison_contract"]
        self.assertEqual(contract["comparison_block_count"], 64)
        self.assertEqual(contract["execution_run_count"], 128)
        self.assertEqual(contract["pairs"], [
            "P43_WHOLE_LEGACY_vs_P50_RAW_II",
            "P44_WHOLE_LEGACY_vs_P50_RAW_II",
        ])
        self.assertTrue(contract["counterbalanced"])
        self.assertTrue(contract["no_cache"])
        for block in contract["blocks"]:
            self.assertEqual(len(block["sequence"]), 2)
            self.assertFalse(block["cache_expected"])
            self.assertTrue(block["byte_identical_required"])
            expected = ["WHOLE_LEGACY", "RAW_II"] if block["order"] == "AB" else ["RAW_II", "WHOLE_LEGACY"]
            self.assertEqual([arm["method"] for arm in block["sequence"]], expected)
            self.assertNotIn("AB", block)
            self.assertNotIn("BA", block)

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

    def test_auditor_rejects_missing_or_reordered_baseline_arm(self) -> None:
        for mutation in ("missing", "reordered"):
            mutant = json.loads(json.dumps(self.plan))
            block = mutant["execution_measurement_contract"]["measurement_cells"][0]
            if mutation == "missing":
                del block["sequence"]
            else:
                block["sequence"].reverse()
            self.assertEqual(audit_plan(mutant)["status"], "FAIL", mutation)
        mutant = json.loads(json.dumps(self.plan))
        mutant["execution_measurement_contract"]["measurement_cells"][0]["AB"] = []
        self.assertEqual(audit_plan(mutant)["status"], "FAIL")
        mutant = json.loads(json.dumps(self.plan))
        block = mutant["homogeneous_version_comparison_contract"]["blocks"][0]
        block["sequence"].reverse()
        self.assertEqual(audit_plan(mutant)["status"], "FAIL")

    def test_auditor_is_total_for_deleted_or_malformed_controls(self) -> None:
        controls = (
            ("source", None),
            ("p44_cache_ambiguity", None),
            ("state_count", None),
            ("transition_count", None),
            ("performance_arms", None),
            ("upgrade_orders", None),
            ("downgrade_orders", None),
            ("boundary_extras", None),
        )
        for key, value in controls:
            for replacement in ("deleted", value):
                mutant = json.loads(json.dumps(self.plan))
                if replacement == "deleted":
                    del mutant[key]
                else:
                    mutant[key] = replacement
                result = audit_plan(mutant)
                self.assertEqual(result["status"], "FAIL", (key, replacement))
                self.assertIsInstance(result["errors"], list)
        for malformed in (None, [], "not-a-plan"):
            result = audit_plan(malformed)
            self.assertEqual(result["status"], "FAIL", malformed)
            self.assertIsInstance(result["errors"], list)

    def test_auditor_rejects_any_canonical_contract_deletion_or_mutation(self) -> None:
        paths = (
            ("transition_class_counts",),
            ("homogeneous_states",),
            ("homogeneous_version_arms",),
            ("optional_unaccepted_method_extras", "ZSTD_COHORT"),
            ("mixed_version_performance",),
            ("migration_orders",),
            ("execution_measurement_contract", "method_contracts", "ZSTD_TU"),
            ("homogeneous_version_comparison_contract", "pairs"),
            ("performance_arms", "p50_raw_ii_whole_legacy"),
            ("upgrade_orders", "43_to_50"),
            ("real_artifact_versions", 0, "source_commit"),
            ("states", 0, "tuple"),
            ("transitions", 0, "classification"),
        )

        def mutate(value: dict, path: tuple[object, ...], *, delete: bool) -> None:
            current = value
            for key in path[:-1]:
                current = current[key]
            if delete:
                del current[path[-1]]
            else:
                current[path[-1]] = None

        for path in paths:
            for delete in (True, False):
                mutant = json.loads(json.dumps(self.plan))
                mutate(mutant, path, delete=delete)
                result = audit_plan(mutant)
                self.assertEqual(result["status"], "FAIL", (path, delete))
                self.assertIsInstance(result["errors"], list)

    def test_cli_is_non_executing_and_emits_json(self) -> None:
        script = Path(__file__).with_name("s4_version_transition_planner.py")
        completed = subprocess.run([sys.executable, str(script), "--summary"],
                                   check=True, capture_output=True, text=True)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(summary["state_count"], 27)
        self.assertEqual(summary["transition_count"], 729)
        self.assertEqual(summary["comparison_block_count"], 128)
        self.assertEqual(summary["execution_run_count"], 256)

    def test_cli_audit_uses_strict_json_parser(self) -> None:
        script = Path(__file__).with_name("s4_version_transition_planner.py")
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "plan.json"
            path.write_text(json.dumps(self.plan, separators=(",", ":")) + "\n")
            completed = subprocess.run([sys.executable, str(script), "--audit", str(path)],
                                       check=False, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0)
            summary = json.loads(completed.stdout)
            self.assertEqual(summary["status"], "PASS")
            self.assertEqual(summary["state_count"], 27)
            self.assertEqual(summary["transition_count"], 729)

            duplicate = path.read_text().replace(
                '"schema":"icecream-s4-version-transition-plan-v1"',
                '"schema":"icecream-s4-version-transition-plan-v1",'
                '"schema":"icecream-s4-version-transition-plan-v1"', 1)
            path.write_text(duplicate)
            completed = subprocess.run([sys.executable, str(script), "--audit", str(path)],
                                       check=False, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 1)
            summary = json.loads(completed.stdout)
            self.assertEqual(summary["status"], "FAIL")
            self.assertIn("duplicate_json_key:schema", summary["errors"])


if __name__ == "__main__":
    unittest.main()
