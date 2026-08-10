#!/usr/bin/env python3
"""Regression tests for the authoritative F-to-S quotient static checker."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
CHECKER = HERE / "f2s_quotient_static_check.py"
MANIFEST = HERE / "f2s-quotient-checks-v1.json"


def run_checker(manifest: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(CHECKER),
            "--manifest",
            str(manifest),
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


class F2SQuotientStaticCheckTests(unittest.TestCase):
    def test_authoritative_three_row_contract_passes(self) -> None:
        result = run_checker(MANIFEST)
        self.assertEqual(result.returncode, 0, result.stdout)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["check_count"], 3)
        self.assertEqual(
            [row["id"] for row in report["checks"]],
            [
                "f2s-quotient-fixed",
                "f2s-quotient-two-frame-witness",
                "f2s-quotient-index-two-mutant",
            ],
        )

    def test_substituted_manifest_is_rejected_even_when_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            substitute = Path(temporary) / MANIFEST.name
            substitute.write_bytes(MANIFEST.read_bytes())
            result = run_checker(substitute)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("only", result.stdout.lower())
        self.assertIn("authoritative", result.stdout.lower())

    def test_counterexample_rows_check_only_their_named_invariant(self) -> None:
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        for row in document["checks"]:
            if row["expected"] != "counterexample":
                continue
            with self.subTest(check=row["id"]):
                config = (HERE / row["config"]).read_text(encoding="utf-8")
                direct = [
                    line.strip()
                    for line in config.splitlines()
                    if line.strip().startswith(("INVARIANT ", "PROPERTY "))
                ]
                self.assertEqual(direct, [f"INVARIANT {row['property']}"])
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
