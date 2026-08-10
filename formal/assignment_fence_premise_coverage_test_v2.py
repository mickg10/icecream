#!/usr/bin/env python3
"""Require fixed, witness, and mutant coverage for every fence premise."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "assignment-fence-proof-checks-v1.json"

PREMISES = {
    "full token identity distinguishes assignments": {
        "core-fixed",
        "core-heterogeneous",
        "core-mixed-token-mutant",
    },
    "release follows a consumed terminal token": {
        "core-fixed",
        "core-release-claimed-mutant",
        "network-release-on-enqueue-mutant",
    },
    "claim and revoke have one ordered linearization outcome": {
        "network-claim-wins-queued-revoke-witness",
        "network-fence-wins-delayed-claim-witness",
        "p49-pipelined-revoke-race",
        "p49-pipelined-revoke-race-witness",
    },
    "F-to-S relative order is FIFO": {
        "network-fixed",
        "network-f2s-bypass-mutant",
    },
    "strict mode observes READY before UseCS": {
        "p49-strict-within-epoch",
        "network-usecs-before-ready-mutant",
    },
    "pipelined mode enqueues PREPARE before UseCS": {
        "p49-pipelined-within-epoch",
        "p49-pipelined-claim-before-prepare-witness",
    },
    "pending claims create no pre-PREPARE side effect": {
        "p49-pipelined-claim-before-prepare",
        "p49-pipelined-side-effect-before-prepare-mutant",
    },
    "pending claim storage is bounded": {
        "p49-pipelined-within-epoch",
        "p49-pipelined-unbounded-pending-mutant",
    },
    "unknown compacted identities are not default-allowed": {
        "network-compaction-fixed",
        "network-default-allow-after-compaction-mutant",
        "p49-pipelined-default-allow-mutant",
    },
    "an already pending exact claim resolves under stated fairness": {
        "p49-pipelined-pending-liveness",
    },
    "the finite FIFO quotient preserves fixed and fair network results": {
        "network-fixed",
        "network-compaction-fixed",
        "network-fair-liveness",
    },
}

EXPECTED_COUNTEREXAMPLES = {
    "core-mixed-token-mutant",
    "core-release-claimed-mutant",
    "network-claim-wins-queued-revoke-witness",
    "network-fence-wins-delayed-claim-witness",
    "network-usecs-before-ready-mutant",
    "network-release-on-enqueue-mutant",
    "network-default-allow-after-compaction-mutant",
    "network-f2s-bypass-mutant",
    "p49-pipelined-claim-before-prepare-witness",
    "p49-pipelined-revoke-race-witness",
    "p49-pipelined-side-effect-before-prepare-mutant",
    "p49-pipelined-unbounded-pending-mutant",
    "p49-pipelined-default-allow-mutant",
}


class PremiseCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cls.rows = {row["id"]: row for row in document["checks"]}

    def test_every_premise_row_exists(self) -> None:
        for premise, check_ids in PREMISES.items():
            with self.subTest(premise=premise):
                self.assertEqual(sorted(check_ids - self.rows.keys()), [])

    def test_mutants_and_witnesses_remain_counterexamples(self) -> None:
        for check_id in sorted(EXPECTED_COUNTEREXAMPLES):
            with self.subTest(check=check_id):
                row = self.rows[check_id]
                self.assertEqual(row["expected"], "counterexample")
                self.assertIn("trace_manifest", row)
                self.assertTrue((HERE / row["trace_manifest"]).is_file())

    def test_each_premise_has_a_fixed_or_progress_row(self) -> None:
        for premise, check_ids in PREMISES.items():
            with self.subTest(premise=premise):
                passing = [
                    check_id
                    for check_id in check_ids
                    if self.rows[check_id]["expected"] == "pass"
                ]
                self.assertTrue(
                    passing,
                    f"{premise}: at least one accepted fixed/progress row required",
                )

    def test_every_focused_network_or_p49_row_is_classified(self) -> None:
        covered = set().union(*PREMISES.values())
        focused_ids = {
            row_id
            for row_id in self.rows
            if row_id.startswith("p49-") or row_id.startswith("network-")
        }
        self.assertEqual(
            sorted(focused_ids - covered),
            [],
            "new focused rows require an explicit named premise",
        )

    def test_coverage_is_not_only_mutants(self) -> None:
        for premise, check_ids in PREMISES.items():
            with self.subTest(premise=premise):
                expectations = {self.rows[check_id]["expected"] for check_id in check_ids}
                self.assertIn("pass", expectations)
                if len(check_ids) > 1:
                    self.assertTrue(
                        "counterexample" in expectations
                        or premise.endswith("fixed and fair network results")
                    )


if __name__ == "__main__":
    unittest.main()
