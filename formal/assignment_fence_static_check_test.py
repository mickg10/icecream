#!/usr/bin/env python3
"""Mutation tests for the proof-bearing assignment-fence static contract."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

from assignment_fence_static_check import ContractError, check_contract

FORMAL_DIR = Path(__file__).resolve().parent
REPO = FORMAL_DIR.parent
MANIFEST = FORMAL_DIR / "assignment-fence-formal-checks-v1.json"


class AssignmentFenceStaticCheckTests(unittest.TestCase):
    def _copy(self) -> tuple[tempfile.TemporaryDirectory[str], Path, Path]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        formal = root / "formal"
        shutil.copytree(FORMAL_DIR, formal)
        return temporary, root, formal

    def test_current_contract_passes(self) -> None:
        result = check_contract(MANIFEST, REPO, FORMAL_DIR)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["check_count"], 14)
        self.assertEqual(result["proof_count"], 1)

    def test_absolute_sequence_history_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        path = formal / "AssignmentFenceNetwork.tla"
        path.write_text(path.read_text() + "\nnextF2SSeq == 1\n", encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "absolute F2S history"):
            check_contract(formal / MANIFEST.name, root, formal)

    def test_missing_receiver_observer_update_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        path = formal / "AssignmentFenceNetwork.tla"
        text = path.read_text(encoding="utf-8")
        fragment = (
            "    /\\ f2sBypassObserved' =\n"
            "          (f2sBypassObserved \\/ (ChosenF2SIndex # 1))\n"
        )
        self.assertGreaterEqual(text.count(fragment), 5)
        text = text.replace(fragment, "", 1)
        path.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "must assign the bypass observer"):
            check_contract(formal / MANIFEST.name, root, formal)

    def test_stale_trace_counter_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        path = (
            formal
            / "trace-manifests"
            / "AssignmentFenceNetworkF2SBypassMutant.manifest.template.json"
        )
        trace = json.loads(path.read_text())
        trace["final_all"][0]["path"] = "nextF2SSeq"
        path.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "retains 'nextF2SSeq'"):
            check_contract(formal / MANIFEST.name, root, formal)

    def test_matrix_reordering_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        path = formal / MANIFEST.name
        document = json.loads(path.read_text())
        document["checks"][0], document["checks"][1] = (
            document["checks"][1],
            document["checks"][0],
        )
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "ids/order mismatch"):
            check_contract(path, root, formal)



    def test_matrix_property_substitution_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        path = formal / MANIFEST.name
        document = json.loads(path.read_text(encoding="utf-8"))
        document["checks"][0]["property"] = "SafetyInvariant"
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "row contract mismatch"):
            check_contract(path, root, formal)

    def test_total_empty_liveness_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        path = formal / "AssignmentFenceNetwork.tla"
        text = path.read_text(encoding="utf-8")
        start = text.index("NoPermanentQueuedFrame ==")
        end = text.index("\n\nFirstWorker ==", start)
        replacement = (
            "NoPermanentQueuedFrame ==\n"
            "    []((Len(s2f) > 0 \\/ Len(f2s) > 0)\n"
            "       => <> (Len(s2f) = 0 /\\ Len(f2s) = 0))"
        )
        path.write_text(text[:start] + replacement + text[end:], encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "per-frame liveness"):
            check_contract(formal / MANIFEST.name, root, formal)

    def test_generic_default_allow_property_is_rejected(self) -> None:
        temporary, root, formal = self._copy()
        self.addCleanup(temporary.cleanup)
        cfg = formal / "AssignmentFenceNetworkDefaultAllowMutant.cfg"
        cfg.write_text(
            cfg.read_text(encoding="utf-8").replace(
                "INVARIANT NoStartAfterRevokedCompaction",
                "INVARIANT NoStartAfterRelease",
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ContractError, "named property directly"):
            check_contract(formal / MANIFEST.name, root, formal)


if __name__ == "__main__":
    unittest.main()
