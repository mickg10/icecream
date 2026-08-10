#!/usr/bin/env python3
"""Offline mutation tests for cross-branch formal evidence collection."""

from __future__ import annotations

import copy
import unittest
from unittest.mock import patch

import collect_cross_branch_formal_evidence as collector


PREREQUISITE = collector.DOMAINS[0]
COMPATIBILITY = collector.DOMAINS[1]


def row(check_id: str, *, expected: str = "pass") -> dict:
    return {
        "id": check_id,
        "workers": 1,
        "toolchains": ["stable", "differential"],
        "expected": expected,
    }


def prerequisite_manifest() -> dict:
    checks = []
    checks.extend(row(f"allocator-{index}") for index in range(5))
    checks.extend(row(f"lifecycle-{index}") for index in range(6))
    checks.extend(row(f"usecs-{index}") for index in range(7))
    checks.extend(row(f"fsession-{index}") for index in range(6))
    return {"schema": 1, "checks": checks, "proofs": []}


def compatibility_manifest() -> dict:
    checks = []
    checks.extend(row(f"compat-fixed-{index}") for index in range(6))
    checks.extend(
        row(f"compat-witness-{index}", expected="counterexample")
        for index in range(16)
    )
    checks.extend(
        row(f"compat-case-{index}-mutant", expected="counterexample")
        for index in range(9)
    )
    return {"schema": 1, "checks": checks, "proofs": []}


def artifact(prefix: str, index: int) -> dict:
    return {
        "id": index,
        "name": f"{prefix}deadbeef",
        "size_in_bytes": 1000 + index,
        "digest": "sha256:" + (f"{index:x}" * 64)[:64],
        "expires_at": "2099-01-01T00:00:00Z",
        "expired": False,
    }


class CrossBranchEvidenceTests(unittest.TestCase):
    def test_read_only_ordinary_workflow_passes(self) -> None:
        text = """name: compatibility-formal
on:
  pull_request:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  static:
    steps:
      - uses: actions/upload-artifact@v4
"""
        collector.validate_workflow(text, COMPATIBILITY)

    def test_write_enabled_workflow_is_rejected(self) -> None:
        text = """workflow_dispatch:
permissions:
  contents: read
  contents: write
jobs:
  static:
    steps:
      - uses: actions/upload-artifact@v4
"""
        with self.assertRaisesRegex(collector.EvidenceError, "write-enabled"):
            collector.validate_workflow(text, COMPATIBILITY)

    def test_repair_job_is_rejected(self) -> None:
        text = """workflow_dispatch:
permissions:
  contents: read
jobs:
  precedence-repair:
    steps:
      - uses: actions/upload-artifact@v4
"""
        with self.assertRaisesRegex(collector.EvidenceError, "repair behavior"):
            collector.validate_workflow(text, COMPATIBILITY)

    def test_prerequisite_matrix_segments_are_exact(self) -> None:
        report = collector.validate_manifest(
            prerequisite_manifest(), PREREQUISITE
        )
        self.assertEqual(report["check_count"], 24)
        document = prerequisite_manifest()
        document["checks"][5]["id"] = "allocator-in-wrong-segment"
        with self.assertRaisesRegex(collector.EvidenceError, "row segment"):
            collector.validate_manifest(document, PREREQUISITE)

    def test_compatibility_last_nine_rows_must_be_mutants(self) -> None:
        report = collector.validate_manifest(
            compatibility_manifest(), COMPATIBILITY
        )
        self.assertEqual(report["check_count"], 31)
        document = compatibility_manifest()
        document["checks"][-1]["id"] = "compat-not-a-mutant"
        with self.assertRaisesRegex(collector.EvidenceError, "final nine"):
            collector.validate_manifest(document, COMPATIBILITY)

    def test_proof_row_in_proofless_domain_is_rejected(self) -> None:
        document = prerequisite_manifest()
        document["proofs"] = [{"id": "invented-proof"}]
        with self.assertRaisesRegex(collector.EvidenceError, "gained proof"):
            collector.validate_manifest(document, PREREQUISITE)

    def test_toolchain_order_drift_is_rejected(self) -> None:
        document = compatibility_manifest()
        document["checks"][0]["toolchains"] = ["differential", "stable"]
        with self.assertRaisesRegex(collector.EvidenceError, "toolchain order"):
            collector.validate_manifest(document, COMPATIBILITY)

    def test_successful_run_requires_exact_name_sha_and_conclusion(self) -> None:
        sha = "a" * 40
        payload = {
            "workflow_runs": [
                {
                    "id": 1,
                    "name": COMPATIBILITY.workflow_name,
                    "head_sha": sha,
                    "status": "completed",
                    "conclusion": "success",
                    "created_at": "2026-01-01T00:00:00Z",
                },
                {
                    "id": 2,
                    "name": COMPATIBILITY.workflow_name,
                    "head_sha": "b" * 40,
                    "status": "completed",
                    "conclusion": "success",
                    "created_at": "2026-01-02T00:00:00Z",
                },
            ]
        }
        with patch.object(collector, "gh", return_value=payload):
            selected = collector.successful_run(
                "mickg10/icecream", sha, COMPATIBILITY
            )
        self.assertEqual(selected["id"], 1)

    def test_failed_or_skipped_job_is_rejected(self) -> None:
        payload = {
            "jobs": [
                {"id": 1, "name": "static", "conclusion": "success"},
                {"id": 2, "name": "tlc", "conclusion": "skipped"},
            ]
        }
        with patch.object(collector, "gh", return_value=payload):
            with self.assertRaisesRegex(collector.EvidenceError, "non-success jobs"):
                collector.validate_jobs(
                    "mickg10/icecream", 1, COMPATIBILITY
                )

    def test_artifact_set_requires_one_live_digest_per_prefix(self) -> None:
        artifacts = [
            artifact(prefix, index + 1)
            for index, prefix in enumerate(COMPATIBILITY.artifact_prefixes)
        ]
        with patch.object(
            collector, "gh", return_value={"artifacts": artifacts}
        ):
            selected = collector.validate_artifacts(
                "mickg10/icecream", 1, COMPATIBILITY
            )
        self.assertEqual(len(selected), 4)

        expired = copy.deepcopy(artifacts)
        expired[0]["expired"] = True
        with patch.object(
            collector, "gh", return_value={"artifacts": expired}
        ):
            with self.assertRaisesRegex(collector.EvidenceError, "expected one live"):
                collector.validate_artifacts(
                    "mickg10/icecream", 1, COMPATIBILITY
                )

        duplicate = artifacts + [copy.deepcopy(artifacts[0])]
        duplicate[-1]["id"] = 99
        with patch.object(
            collector, "gh", return_value={"artifacts": duplicate}
        ):
            with self.assertRaisesRegex(collector.EvidenceError, "expected one live"):
                collector.validate_artifacts(
                    "mickg10/icecream", 1, COMPATIBILITY
                )

        missing_digest = copy.deepcopy(artifacts)
        missing_digest[0]["digest"] = None
        with patch.object(
            collector, "gh", return_value={"artifacts": missing_digest}
        ):
            with self.assertRaisesRegex(collector.EvidenceError, "digest required"):
                collector.validate_artifacts(
                    "mickg10/icecream", 1, COMPATIBILITY
                )


if __name__ == "__main__":
    unittest.main()
