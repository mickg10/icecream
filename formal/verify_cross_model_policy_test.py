#!/usr/bin/env python3
"""Mutation tests for verify_cross_model_policy.py."""

from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

import verify_cross_model_policy as verifier

HERE = Path(__file__).resolve().parent


MODEL = r'''---------------- MODULE MixedVersionCompatibility ----------------
OldVersion == 1
NewVersion == 2
ExpectedPolicy(c, w) ==
    IF WorkerVersion[w] = OldVersion
    THEN "Legacy"
    ELSE IF ClientVersion[c] = OldVersion
         THEN "FencedLegacy"
         ELSE "Token"
GuaranteeOf(p) ==
    CASE p = "Legacy" -> "LegacyOnly"
      [] p = "FencedLegacy" -> "EpochScoped"
      [] p = "Token" -> "ExactRestart"
      [] OTHER -> "None"
IdentityOf(p) == IF p = "Token" THEN "FullToken" ELSE "Wire"
PerAssignmentPolicyCorrect == TRUE
GuaranteeMatchesPolicy == TRUE
ClaimIdentityMatchesPolicy == TRUE
OldWorkerSeesOnlyOldVocabulary == TRUE
OldClientSeesNoTokenField == TRUE
NoGlobalCapabilityLeak == TRUE
TokenRejectsStaleRestartClaim == TRUE
OldTraceRefinement == TRUE
PolicyChosenBeforeDispatch == TRUE
=====================================================================
'''

HANDOFF = """# Mixed-version compatibility final handoff
bigoracle/compatibility-formal
31 rows
S'FC
S'F'C
S'F'C'
S'FC'
S'F[F']C[C']
S'F'[CC']
"""

REQUIRED_IDS = [
    "compat-old-f-old-c",
    "compat-new-f-old-c",
    "compat-new-f-new-c",
    "compat-old-f-new-c",
    "compat-mixed-f-mixed-c",
    "compat-new-f-mixed-c",
    "compat-legacy-restart-limitation-witness",
    "compat-fenced-restart-limitation-witness",
    "compat-token-restart-reject-witness",
    "compat-token-old-client-mutant",
    "compat-global-capability-mutant",
    "compat-policy-after-dispatch-mutant",
]


def manifest() -> dict:
    rows = []
    for check_id in REQUIRED_IDS:
        witness = "witness" in check_id or check_id.endswith("-mutant")
        row = {
            "id": check_id,
            "expected": "counterexample" if witness else "pass",
        }
        if check_id in {
            "compat-legacy-restart-limitation-witness",
            "compat-fenced-restart-limitation-witness",
            "compat-token-restart-reject-witness",
        }:
            row["trace_manifest"] = f"trace-manifests/{check_id}.json"
        rows.append(row)
    while len(rows) < 31:
        rows.append({"id": f"compat-filler-{len(rows)}", "expected": "pass"})
    return {"schema": 1, "checks": rows, "proofs": []}


class CrossModelPolicyTests(unittest.TestCase):
    def make_trees(self, temporary: str) -> tuple[Path, Path]:
        root = Path(temporary)
        consumer = root / "consumer"
        compatibility = root / "compatibility"
        (consumer / "formal").mkdir(parents=True)
        (compatibility / "formal" / "trace-manifests").mkdir(parents=True)
        (consumer / "formal" / "ASSIGNMENT_FENCE_THEORY.md").write_bytes(
            (HERE / "ASSIGNMENT_FENCE_THEORY.md").read_bytes()
        )
        (consumer / "formal" / "UPSTREAM_LANDING_PLAN.md").write_bytes(
            (HERE / "UPSTREAM_LANDING_PLAN.md").read_bytes()
        )
        (compatibility / "formal" / "MixedVersionCompatibility.tla").write_text(
            MODEL, encoding="utf-8"
        )
        (compatibility / "formal" / "COMPATIBILITY_FINAL_HANDOFF.md").write_text(
            HANDOFF, encoding="utf-8"
        )
        (compatibility / "formal" / "compatibility-formal-checks-v1.json").write_text(
            json.dumps(manifest(), indent=2) + "\n", encoding="utf-8"
        )
        (compatibility / "formal" / "compatibility_codec_fixture.py").write_text(
            '"""This is not the production Icecream codec."""\n',
            encoding="utf-8",
        )
        (compatibility / "formal" / "compatibility_codec_fixture_test.py").write_text(
            "# real fixture contract test placeholder for unit construction\n",
            encoding="utf-8",
        )
        return consumer, compatibility

    def test_consistent_policy_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            report = verifier.verify(consumer, compatibility)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["policy"]["old_f"], "Legacy")
        self.assertEqual(report["policy"]["new_f_old_c"], "FencedLegacy")
        self.assertEqual(report["policy"]["new_f_new_c"], "Token")
        self.assertEqual(report["compatibility"]["row_count"], 31)

    def test_old_worker_token_policy_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            path = compatibility / "formal" / "MixedVersionCompatibility.tla"
            path.write_text(
                MODEL.replace('THEN "Legacy"', 'THEN "Token"', 1),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(verifier.PolicyError, "ExpectedPolicy"):
                verifier.verify(consumer, compatibility)

    def test_missing_old_peer_property_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            path = compatibility / "formal" / "MixedVersionCompatibility.tla"
            path.write_text(
                MODEL.replace("OldClientSeesNoTokenField == TRUE\n", ""),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(verifier.PolicyError, "property absent"):
                verifier.verify(consumer, compatibility)

    def test_missing_restart_limitation_witness_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            document = manifest()
            document["checks"] = [
                row
                for row in document["checks"]
                if row["id"] != "compat-fenced-restart-limitation-witness"
            ]
            document["checks"].append(
                {"id": "compat-replacement-filler", "expected": "pass"}
            )
            path = compatibility / "formal" / "compatibility-formal-checks-v1.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(verifier.PolicyError, "rows missing"):
                verifier.verify(consumer, compatibility)

    def test_limitation_silently_changed_to_pass_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            document = manifest()
            row = next(
                row
                for row in document["checks"]
                if row["id"] == "compat-legacy-restart-limitation-witness"
            )
            row["expected"] = "pass"
            path = compatibility / "formal" / "compatibility-formal-checks-v1.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(verifier.PolicyError, "verdict drifted"):
                verifier.verify(consumer, compatibility)

    def test_missing_topology_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            path = compatibility / "formal" / "COMPATIBILITY_FINAL_HANDOFF.md"
            path.write_text(HANDOFF.replace("S'F'[CC']\n", ""), encoding="utf-8")
            with self.assertRaisesRegex(verifier.PolicyError, "topology absent"):
                verifier.verify(consumer, compatibility)

    def test_generated_fixture_must_retain_nonproduction_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            consumer, compatibility = self.make_trees(temporary)
            path = compatibility / "formal" / "compatibility_codec_fixture.py"
            path.write_text('"""codec"""\n', encoding="utf-8")
            with self.assertRaisesRegex(verifier.PolicyError, "non-production"):
                verifier.verify(consumer, compatibility)


if __name__ == "__main__":
    unittest.main()
