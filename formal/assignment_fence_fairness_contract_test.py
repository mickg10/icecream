#!/usr/bin/env python3
"""Contract tests for assignment-fence fairness and liveness nonvacuity."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "assignment-fence-proof-checks-v1.json"

LIVENESS_SUPPORT = {
    "core-revoked-result-liveness": {
        "core-release-claimed-mutant",
    },
    "network-fair-liveness": {
        "network-claim-wins-queued-revoke-witness",
        "network-fence-wins-delayed-claim-witness",
    },
    "p49-pipelined-pending-liveness": {
        "p49-pipelined-claim-before-prepare-witness",
    },
}

FORBIDDEN_FAIR_ACTION_FRAGMENTS = (
    "CompleteCompile",
    "CompilerComplete",
    "WorkerLoss",
    "SessionLoss",
    "ConnectionLoss",
    "Restart",
    "SubmitRequest",
    "RequestAssignment",
    "StartCompile",
)

WF_RE = re.compile(
    r"WF_[A-Za-z_][A-Za-z0-9_]*\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)"
)
SF_RE = re.compile(r"SF_[A-Za-z_][A-Za-z0-9_]*\s*\(")
SPEC_RE = re.compile(r"(?m)^\s*SPECIFICATION\s+(\w+)\s*$")
PROPERTY_RE = re.compile(r"(?m)^\s*PROPERTY\s+(\w+)\s*$")


class AssignmentFenceFairnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cls.rows = {row["id"]: row for row in document["checks"]}

    def module_text(self, row: dict) -> str:
        return (HERE / f"{row['module']}.tla").read_text(encoding="utf-8")

    def config_text(self, row: dict) -> str:
        return (HERE / row["config"]).read_text(encoding="utf-8")

    def test_exact_liveness_row_inventory(self) -> None:
        actual = {
            check_id
            for check_id, row in self.rows.items()
            if row["kind"] == "liveness"
        }
        self.assertEqual(actual, set(LIVENESS_SUPPORT))

    def test_liveness_configs_use_fair_spec_and_one_direct_property(self) -> None:
        for check_id in sorted(LIVENESS_SUPPORT):
            row = self.rows[check_id]
            with self.subTest(check=check_id):
                self.assertEqual(row["workers"], 1)
                config = self.config_text(row)
                self.assertEqual(SPEC_RE.findall(config), ["FairSpec"])
                self.assertEqual(PROPERTY_RE.findall(config), [row["property"]])
                self.assertNotIn("INVARIANT ", config)

    def test_fairness_is_weak_narrow_and_not_an_environment_assumption(self) -> None:
        for check_id in sorted(LIVENESS_SUPPORT):
            row = self.rows[check_id]
            with self.subTest(check=check_id):
                text = self.module_text(row)
                self.assertIsNone(SF_RE.search(text), "strong fairness is forbidden")
                actions = WF_RE.findall(text)
                self.assertTrue(actions, "FairSpec must expose at least one direct WF action")
                for action in actions:
                    self.assertFalse(
                        any(fragment in action for fragment in FORBIDDEN_FAIR_ACTION_FRAGMENTS),
                        f"environment outcome {action} cannot be assumed fair",
                    )
                if check_id == "p49-pipelined-pending-liveness":
                    self.assertEqual(set(actions), {"ConsumePrepare"})

    def test_manifest_states_the_fairness_boundary(self) -> None:
        for check_id in sorted(LIVENESS_SUPPORT):
            row = self.rows[check_id]
            with self.subTest(check=check_id):
                assumptions = row.get("assumptions")
                self.assertIsInstance(assumptions, list)
                self.assertTrue(assumptions)
                joined = " ".join(assumptions).lower()
                self.assertIn("weak fairness", joined)
                for forbidden in (
                    "assume compiler completion",
                    "compiler completion is assumed",
                    "assume worker loss",
                    "worker loss is assumed",
                    "assume connection repair",
                    "connection repair is assumed",
                ):
                    self.assertNotIn(forbidden, joined)

    def test_every_liveness_theorem_has_reachable_support(self) -> None:
        for check_id, support_ids in LIVENESS_SUPPORT.items():
            with self.subTest(check=check_id):
                self.assertTrue(support_ids)
                for support_id in support_ids:
                    self.assertIn(support_id, self.rows)
                    support = self.rows[support_id]
                    self.assertEqual(support["expected"], "counterexample")
                    trace_relative = support.get("trace_manifest")
                    self.assertIsInstance(trace_relative, str)
                    trace = json.loads(
                        (HERE / trace_relative).read_text(encoding="utf-8")
                    )
                    self.assertTrue(trace["required_subsequence"])
                    self.assertTrue(
                        trace.get("final_all") or trace.get("require_lasso") is True
                    )
                    self.assertTrue(trace["harness_steps"])


if __name__ == "__main__":
    unittest.main()
