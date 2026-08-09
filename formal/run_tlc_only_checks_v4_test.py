#!/usr/bin/env python3
"""Self-tests for the proofless generation-4 TLC orchestration."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

MODULE_PATH = Path(__file__).with_name("run_tlc_only_checks_v4.py")
SPEC = importlib.util.spec_from_file_location("run_tlc_only_checks_v4", MODULE_PATH)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)


class ProoflessRunnerContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    @staticmethod
    def manifest(*, proofs: list[object] | None = None) -> dict[str, object]:
        return {
            "schema": 1,
            "checks": [
                {
                    "id": "fixed",
                    "module": "M",
                    "config": "M.cfg",
                    "kind": "safety",
                    "expected": "pass",
                    "property": "Inv",
                    "workers": 1,
                    "toolchains": ["stable", "differential"],
                }
            ],
            "proofs": [] if proofs is None else proofs,
        }

    def test_explicit_empty_proofs_is_accepted(self) -> None:
        checked = module._validate_proofless_manifest(self.manifest())
        self.assertEqual(checked["proofs"], [])

    def test_proof_row_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            module.TlcOnlyContractError, "explicit empty proofs"
        ):
            module._validate_proofless_manifest(
                self.manifest(proofs=[{"id": "proof", "file": "P.tla"}])
            )

    def test_toolchain_order_is_part_of_contract(self) -> None:
        document = self.manifest()
        document["checks"][0]["toolchains"] = ["differential", "stable"]
        with self.assertRaisesRegex(
            module.TlcOnlyContractError, "stable then differential"
        ):
            module._validate_proofless_manifest(document)

    def test_only_selection_is_fail_closed(self) -> None:
        checks = self.manifest()["checks"]
        selected = module._selected_checks(checks, ["fix.*"])
        self.assertEqual([item["id"] for item in selected], ["fixed"])
        with self.assertRaisesRegex(
            module.TlcOnlyContractError, "selected no checks"
        ):
            module._selected_checks(checks, ["mutant.*"])

    def test_artifacts_must_be_outside_checkout(self) -> None:
        repo = self.root / "repo"
        repo.mkdir()
        self.assertTrue(module._inside(repo / "artifacts", repo))
        self.assertFalse(module._inside(self.root / "external", repo))

    def test_parser_has_no_tlaps_inventory_arguments(self) -> None:
        parser = module._build_parser("test")
        destinations = {action.dest for action in parser._actions}
        self.assertIn("stable_jar", destinations)
        self.assertIn("differential_jar", destinations)
        self.assertNotIn("tlapm", destinations)
        self.assertNotIn("backend", destinations)
        self.assertNotIn("skip_proofs", destinations)

    def test_domain_wrappers_pin_exact_manifest_and_static_checker(self) -> None:
        prerequisite = Path(__file__).with_name("run_prerequisite_checks_v4.py")
        compatibility = Path(__file__).with_name("run_compatibility_checks_v4.py")
        prerequisite_text = prerequisite.read_text(encoding="utf-8")
        compatibility_text = compatibility.read_text(encoding="utf-8")
        self.assertIn(
            'required_manifest_name="prerequisite-formal-checks-v4.json"',
            prerequisite_text,
        )
        self.assertIn(
            'static_check_name="prerequisite_static_check_v2.py"',
            prerequisite_text,
        )
        self.assertIn(
            'required_manifest_name="compatibility-formal-checks-v1.json"',
            compatibility_text,
        )
        self.assertIn(
            'static_check_name="compatibility_static_check.py"',
            compatibility_text,
        )


if __name__ == "__main__":
    unittest.main()
