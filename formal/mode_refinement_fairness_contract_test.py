#!/usr/bin/env python3
"""Contract tests for mode-refinement fairness and abstraction witnesses."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "mode-refinement-checks-v1.json"
MODEL = HERE / "AssignmentFenceModeRefinement.tla"

WF_RE = re.compile(
    r"WF_[A-Za-z_][A-Za-z0-9_]*\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)"
)
SF_RE = re.compile(r"SF_[A-Za-z_][A-Za-z0-9_]*\s*\(")
SPEC_RE = re.compile(r"(?m)^\s*SPECIFICATION\s+(\w+)\s*$")
PROPERTY_RE = re.compile(r"(?m)^\s*PROPERTY\s+(\w+)\s*$")


class ModeRefinementFairnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cls.rows = {row["id"]: row for row in document["checks"]}
        cls.model = MODEL.read_text(encoding="utf-8")

    def test_exactly_one_liveness_row_exists(self) -> None:
        liveness = {
            check_id
            for check_id, row in self.rows.items()
            if row["kind"] == "liveness"
        }
        self.assertEqual(liveness, {"mode-refinement-pending-liveness"})

    def test_fair_spec_is_only_weak_fair_consume_prepare(self) -> None:
        self.assertIsNone(SF_RE.search(self.model), "strong fairness is forbidden")
        self.assertEqual(set(WF_RE.findall(self.model)), {"ConsumePrepare"})
        self.assertIn(
            "FairSpec == Spec /\\ WF_vars(ConsumePrepare)",
            self.model,
        )
        for forbidden in (
            "WF_vars(StartCompile)",
            "WF_vars(FinishCompile)",
            "WF_vars(QueueRevoke)",
            "WF_vars(ConsumeRevoke)",
            "WF_vars(Release)",
        ):
            self.assertNotIn(forbidden, self.model)

    def test_liveness_config_checks_only_the_named_property(self) -> None:
        row = self.rows["mode-refinement-pending-liveness"]
        config = (HERE / row["config"]).read_text(encoding="utf-8")
        self.assertEqual(SPEC_RE.findall(config), ["FairSpec"])
        self.assertEqual(PROPERTY_RE.findall(config), [row["property"]])
        self.assertNotIn("INVARIANT ", config)
        self.assertEqual(row["workers"], 1)
        assumptions = " ".join(row["assumptions"]).lower()
        self.assertIn("weak fairness", assumptions)
        self.assertIn("consumeprepare", assumptions)
        for forbidden in (
            "compiler completion is assumed",
            "peer loss is assumed",
            "restart is assumed",
            "connection repair is assumed",
        ):
            self.assertNotIn(forbidden, assumptions)

    def test_pending_liveness_is_nonvacuous(self) -> None:
        witness = self.rows["mode-refinement-pending-stutter-witness"]
        self.assertEqual(witness["expected"], "counterexample")
        trace = json.loads(
            (HERE / witness["trace_manifest"]).read_text(encoding="utf-8")
        )
        self.assertIn("PendingClaimIsAbstractStutter", trace["required_subsequence"])
        final = {
            predicate["path"]: predicate.get("eq")
            for predicate in trace["final_all"]
        }
        self.assertEqual(final.get("pendingCount"), 1)
        self.assertEqual(final.get("matched"), False)
        self.assertEqual(final.get("authorizationSeen"), False)
        self.assertEqual(final.get("abstractState"), "Awaiting")

    def test_stutter_and_revoke_witnesses_cover_both_abstract_outcomes(self) -> None:
        stutter = self.rows["mode-refinement-pending-stutter-witness"]
        revoke = self.rows["mode-refinement-revoke-witness"]
        self.assertEqual(stutter["expected"], "counterexample")
        self.assertEqual(revoke["expected"], "counterexample")
        stutter_trace = json.loads(
            (HERE / stutter["trace_manifest"]).read_text(encoding="utf-8")
        )
        revoke_trace = json.loads(
            (HERE / revoke["trace_manifest"]).read_text(encoding="utf-8")
        )
        self.assertTrue(
            any(
                item.get("path") == "abstractState"
                and item.get("eq") == "Awaiting"
                for item in stutter_trace["final_all"]
            )
        )
        self.assertTrue(
            any(
                item.get("path") == "abstractState"
                and item.get("eq") == "Revoked"
                for item in revoke_trace["final_all"]
            )
        )

    def test_every_refinement_failure_has_a_direct_mutant(self) -> None:
        expected = {
            "mode-refinement-pending-authorizes-mutant": "PendingDoesNotAuthorize",
            "mode-refinement-start-before-grant-mutant": "StartRequiresAuthorization",
            "mode-refinement-drop-revoke-mutant": "AbstractionRelation",
            "mode-refinement-unbounded-pending-mutant": "PendingBound",
        }
        for check_id, property_name in expected.items():
            with self.subTest(check=check_id):
                row = self.rows[check_id]
                self.assertEqual(row["expected"], "counterexample")
                self.assertEqual(row["property"], property_name)
                self.assertTrue((HERE / row["trace_manifest"]).is_file())


if __name__ == "__main__":
    unittest.main()
