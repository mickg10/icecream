#!/usr/bin/env python3
"""Repository-level self-test for compatibility_static_check.py."""

from __future__ import annotations

import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True

FORMAL = Path(__file__).resolve().parent
REPO = FORMAL.parent
SCRIPT = FORMAL / "compatibility_static_check.py"
SPEC = importlib.util.spec_from_file_location("compatibility_static_check", SCRIPT)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


class CompatibilityStaticContractTests(unittest.TestCase):
    def test_exact_repository_matrix_and_codec_pass_static_contract(self) -> None:
        output = io.StringIO()
        error = io.StringIO()
        with redirect_stdout(output), redirect_stderr(error):
            result = checker.main(
                [
                    "--manifest",
                    str(FORMAL / "compatibility-formal-checks-v1.json"),
                    "--repo",
                    str(REPO),
                    "--formal-dir",
                    str(FORMAL),
                ]
            )
        self.assertEqual(result, 0, error.getvalue() + output.getvalue())
        self.assertIn('"check_count": 31', output.getvalue())
        self.assertIn('"status": "PASS"', output.getvalue())
        self.assertIn('"returncode": 0', output.getvalue())

    def test_wrong_manifest_path_is_rejected(self) -> None:
        output = io.StringIO()
        error = io.StringIO()
        with redirect_stdout(output), redirect_stderr(error):
            result = checker.main(
                [
                    "--manifest",
                    str(FORMAL / "formal-checks.json"),
                    "--repo",
                    str(REPO),
                    "--formal-dir",
                    str(FORMAL),
                ]
            )
        self.assertEqual(result, 1)
        self.assertIn("only", error.getvalue())

    def test_internal_group_contract_is_exact(self) -> None:
        self.assertEqual(len(checker.FIXED_IDS), 6)
        self.assertEqual(len(checker.WITNESS_IDS), 16)
        self.assertEqual(len(checker.MUTANT_IDS), 9)
        self.assertEqual(len(checker.EXPECTED_IDS), 31)
        self.assertEqual(len(set(checker.EXPECTED_IDS)), 31)


if __name__ == "__main__":
    unittest.main()
