#!/usr/bin/env python3
"""Mutation tests for mode_refinement_static_check.py."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import mode_refinement_static_check as checker

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
MANIFEST = HERE / "mode-refinement-checks-v1.json"
SCRIPT = HERE / "mode_refinement_static_check.py"


class ModeRefinementStaticCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def test_authoritative_nine_row_contract_passes(self) -> None:
        report = checker.check_document(copy.deepcopy(self.document), HERE)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["check_count"], 9)
        self.assertEqual(
            [row["id"] for row in report["checks"]],
            [expected[0] for expected in checker.EXPECTED_ROWS],
        )
        enabled = {
            row["id"]: row["enabled_mutant"]
            for row in report["checks"]
            if row["enabled_mutant"] is not None
        }
        self.assertEqual(
            enabled,
            {
                "mode-refinement-pending-authorizes-mutant": "MutantPendingAuthorizes",
                "mode-refinement-start-before-grant-mutant": "MutantStartBeforeGrant",
                "mode-refinement-drop-revoke-mutant": "MutantDropRevoke",
                "mode-refinement-unbounded-pending-mutant": "MutantUnboundedPending",
            },
        )

    def test_byte_identical_substituted_manifest_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            substitute = Path(temporary) / MANIFEST.name
            substitute.write_bytes(MANIFEST.read_bytes())
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--manifest",
                    str(substitute),
                    "--repo",
                    str(REPO),
                    "--formal-dir",
                    "formal",
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("only", result.stdout.lower())
        self.assertIn("authoritative", result.stdout.lower())

    def assert_rejected(self, document: dict, fragment: str) -> None:
        with self.assertRaises(checker.StaticError) as raised:
            checker.check_document(document, HERE)
        self.assertIn(fragment.lower(), str(raised.exception).lower())

    def test_row_reordering_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["checks"][2], document["checks"][3] = (
            document["checks"][3],
            document["checks"][2],
        )
        self.assert_rejected(document, "order")

    def test_proof_row_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["proofs"] = [
            {
                "id": "invented-refinement-proof",
                "file": "AssignmentFenceCoreProof.tla",
                "expected": "pass",
            }
        ]
        self.assert_rejected(document, "no proof rows")

    def test_reversed_toolchain_order_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["checks"][0]["toolchains"] = ["differential", "stable"]
        self.assert_rejected(document, "stable/differential order")

    def test_counterexample_without_trace_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        row = next(
            row for row in document["checks"] if row["expected"] == "counterexample"
        )
        row.pop("trace_manifest")
        self.assert_rejected(document, "trace path mismatch")

    def test_passing_row_with_trace_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        passing = next(row for row in document["checks"] if row["expected"] == "pass")
        passing["trace_manifest"] = (
            "trace-manifests/ModeRefinementRevokeWitness.manifest.template.json"
        )
        self.assert_rejected(document, "unexpected trace")

    def test_liveness_row_cannot_be_relabelled_safety(self) -> None:
        document = copy.deepcopy(self.document)
        row = document["checks"][-1]
        row["kind"] = "safety"
        self.assert_rejected(document, "kind mismatch")

    def test_worker_count_other_than_one_is_rejected(self) -> None:
        document = copy.deepcopy(self.document)
        document["checks"][1]["workers"] = 2
        self.assert_rejected(document, "one worker")

    def test_every_expected_counterexample_has_nonempty_trace_contract(self) -> None:
        for row in self.document["checks"]:
            if row["expected"] != "counterexample":
                continue
            with self.subTest(check=row["id"]):
                trace = json.loads(
                    (HERE / row["trace_manifest"]).read_text(encoding="utf-8")
                )
                self.assertEqual(trace["property"], row["property"])
                self.assertEqual(trace["metadata"]["config"], row["config"])
                self.assertTrue(trace["required_subsequence"])
                self.assertTrue(trace["final_all"])
                self.assertTrue(trace["harness_steps"])


if __name__ == "__main__":
    unittest.main()
