#!/usr/bin/env python3
"""Require fixed, witness, mutant, and progress coverage per refinement premise."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "mode-refinement-checks-v1.json"

PREMISES = {
    "strict concrete state refines the abstract authorization machine": {
        "mode-refinement-strict-fixed",
    },
    "pipelined pending claim is an abstract stutter": {
        "mode-refinement-pipelined-fixed",
        "mode-refinement-pending-stutter-witness",
        "mode-refinement-pending-authorizes-mutant",
    },
    "compiler start requires prior exact abstract authorization": {
        "mode-refinement-pipelined-fixed",
        "mode-refinement-start-before-grant-mutant",
    },
    "concrete fence-winning revoke maps to abstract Revoked": {
        "mode-refinement-pipelined-fixed",
        "mode-refinement-revoke-witness",
        "mode-refinement-drop-revoke-mutant",
    },
    "pending claim storage remains bounded": {
        "mode-refinement-pipelined-fixed",
        "mode-refinement-unbounded-pending-mutant",
    },
    "an already pending claim resolves under weak PREPARE-consume fairness": {
        "mode-refinement-pending-stutter-witness",
        "mode-refinement-pending-liveness",
    },
}

COUNTEREXAMPLES = {
    "mode-refinement-pending-stutter-witness",
    "mode-refinement-revoke-witness",
    "mode-refinement-pending-authorizes-mutant",
    "mode-refinement-start-before-grant-mutant",
    "mode-refinement-drop-revoke-mutant",
    "mode-refinement-unbounded-pending-mutant",
}


class ModeRefinementPremiseCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cls.rows = {row["id"]: row for row in document["checks"]}

    def test_every_named_premise_row_exists(self) -> None:
        for premise, check_ids in PREMISES.items():
            with self.subTest(premise=premise):
                self.assertEqual(sorted(check_ids - self.rows.keys()), [])

    def test_every_matrix_row_is_classified(self) -> None:
        covered = set().union(*PREMISES.values())
        self.assertEqual(sorted(self.rows.keys() - covered), [])

    def test_each_safety_premise_has_a_passing_fixed_row(self) -> None:
        for premise, check_ids in PREMISES.items():
            with self.subTest(premise=premise):
                passing = [
                    check_id
                    for check_id in check_ids
                    if self.rows[check_id]["expected"] == "pass"
                ]
                self.assertTrue(passing, f"{premise}: fixed/progress row required")

    def test_every_discriminating_row_has_a_trace_contract(self) -> None:
        for check_id in sorted(COUNTEREXAMPLES):
            with self.subTest(check=check_id):
                row = self.rows[check_id]
                self.assertEqual(row["expected"], "counterexample")
                trace_relative = row.get("trace_manifest")
                self.assertIsInstance(trace_relative, str)
                trace = json.loads(
                    (HERE / trace_relative).read_text(encoding="utf-8")
                )
                self.assertEqual(trace["property"], row["property"])
                self.assertEqual(trace["metadata"]["config"], row["config"])
                self.assertTrue(trace["required_subsequence"])
                self.assertTrue(trace["final_all"])
                self.assertTrue(trace["harness_steps"])

    def test_mutants_are_one_premise_each(self) -> None:
        mutant_to_property = {
            "mode-refinement-pending-authorizes-mutant": "PendingDoesNotAuthorize",
            "mode-refinement-start-before-grant-mutant": "StartRequiresAuthorization",
            "mode-refinement-drop-revoke-mutant": "AbstractionRelation",
            "mode-refinement-unbounded-pending-mutant": "PendingBound",
        }
        for check_id, property_name in mutant_to_property.items():
            with self.subTest(check=check_id):
                row = self.rows[check_id]
                self.assertEqual(row["property"], property_name)
                config = (HERE / row["config"]).read_text(encoding="utf-8")
                enabled = [
                    line.split("=", 1)[0].strip()
                    for line in config.splitlines()
                    if line.strip().startswith("Mutant")
                    and line.strip().endswith("= TRUE")
                ]
                self.assertEqual(len(enabled), 1)


if __name__ == "__main__":
    unittest.main()
