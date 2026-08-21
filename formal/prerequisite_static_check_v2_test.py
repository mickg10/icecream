#!/usr/bin/env python3
"""Repository-level self-test for prerequisite_static_check_v2.py."""

from __future__ import annotations

import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True

FORMAL = Path(__file__).resolve().parent
REPO = FORMAL.parent
SCRIPT = FORMAL / "prerequisite_static_check_v2.py"
SPEC = importlib.util.spec_from_file_location("prerequisite_static_check_v2", SCRIPT)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


class PrerequisiteStaticContractTests(unittest.TestCase):
    def test_exact_repository_matrix_passes_static_contract(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            result = checker.main(
                [
                    "--manifest",
                    str(FORMAL / "prerequisite-formal-checks-v4.json"),
                    "--repo",
                    str(REPO),
                    "--formal-dir",
                    str(FORMAL),
                ]
            )
        self.assertEqual(result, 0, output.getvalue())
        self.assertIn('"check_count": 24', output.getvalue())
        self.assertIn('"status": "PASS"', output.getvalue())

    def test_wrong_manifest_path_is_rejected(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
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


if __name__ == "__main__":
    unittest.main()
