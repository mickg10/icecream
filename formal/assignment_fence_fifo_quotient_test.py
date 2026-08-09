#!/usr/bin/env python3
"""Static regression contract for the finite F->S FIFO quotient.

These tests supplement SANY/TLC.  They make the state-space reduction and the
load-bearing liveness/trace wiring reviewable without running Java, and reject
accidental restoration of an unbounded absolute sequence history.
"""

from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

FORMAL = Path(__file__).resolve().parent
NETWORK = FORMAL / "AssignmentFenceNetwork.tla"
COMPACTION = FORMAL / "AssignmentFenceNetworkCompaction.tla"
MATRIX = FORMAL / "formal-checks.json"
BYPASS_CFG = FORMAL / "AssignmentFenceNetworkF2SBypassMutant.cfg"
BYPASS_MANIFEST = (
    FORMAL
    / "trace-manifests"
    / "AssignmentFenceNetworkF2SBypassMutant.manifest.template.json"
)
DEFAULT_CFG = FORMAL / "AssignmentFenceNetworkDefaultAllowMutant.cfg"
DEFAULT_MANIFEST = (
    FORMAL
    / "trace-manifests"
    / "AssignmentFenceNetworkDefaultAllowMutant.manifest.template.json"
)


def operator_block(text: str, name: str) -> str:
    start_pattern = re.compile(
        rf"(?m)^{re.escape(name)}(?:\([^\n]*\))?\s*=="
    )
    start = start_pattern.search(text)
    if start is None:
        raise AssertionError(f"missing operator {name}")
    next_pattern = re.compile(
        r"(?m)^[A-Za-z][A-Za-z0-9_]*(?:\([^\n]*\))?\s*=="
    )
    following = next_pattern.search(text, start.end())
    end = following.start() if following is not None else len(text)
    return text[start.start():end]


class FifoQuotientContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.network = NETWORK.read_text(encoding="utf-8")
        cls.compaction = COMPACTION.read_text(encoding="utf-8")
        cls.matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
        cls.bypass_manifest = json.loads(BYPASS_MANIFEST.read_text(encoding="utf-8"))
        cls.default_manifest = json.loads(DEFAULT_MANIFEST.read_text(encoding="utf-8"))

    def test_absolute_sequence_ghost_is_absent(self) -> None:
        combined = self.network + self.compaction
        for forbidden in ("nextF2SSeq", "lastF2SConsumed", ".seq"):
            self.assertNotIn(forbidden, combined)
        self.assertIn("Msg(kind, assignment) ==", self.network)
        message_type = operator_block(self.network, "MessageType")
        self.assertNotIn("Nat", message_type)
        self.assertNotIn("seq", message_type)

    def test_fixed_consumers_record_only_non_head_selection(self) -> None:
        chosen = operator_block(self.network, "ChosenF2SIndex")
        self.assertIn("IF MutantF2SBypass /\\ Len(f2s) >= 2 THEN 2 ELSE 1", chosen)
        consumers = (
            "SReceiveReady",
            "SReceiveBegin",
            "SReceiveRevoked",
            "SReceiveOwned",
            "SReceiveDone",
        )
        assignment = "f2sBypassObserved \\/ (ChosenF2SIndex # 1)"
        for name in consumers:
            with self.subTest(action=name):
                block = operator_block(self.network, name)
                self.assertIn("f2sBypassObserved'", block)
                self.assertIn(assignment, block)
                self.assertIn("RemoveAt(f2s, ChosenF2SIndex)", block)

    def test_bypass_property_and_trace_are_direct(self) -> None:
        prop = operator_block(self.network, "F2SStrictlyIncreasing")
        self.assertIn("~f2sBypassObserved", prop)
        self.assertIn("INVARIANT F2SStrictlyIncreasing", BYPASS_CFG.read_text())
        self.assertEqual(self.bypass_manifest["property"], "F2SStrictlyIncreasing")
        self.assertEqual(
            self.bypass_manifest["required_subsequence"],
            [
                "SQueuePrepareA0",
                "SQueuePrepareA1",
                "FQueueReadyA0AsPredecessor",
                "FQueueReadyA1BehindA0",
                "SConsumeReadyA1BeforeA0",
            ],
        )
        self.assertIn(
            {"path": "f2sBypassObserved", "eq": True},
            self.bypass_manifest["final_all"],
        )

    def test_unique_finite_keys_support_per_frame_liveness(self) -> None:
        unique = operator_block(self.network, "QueueEntriesUnique")
        for queue in ("s2f", "f2s", "s2d", "c2f"):
            self.assertIn(f"Cardinality(SeqElems({queue})) = Len({queue})", unique)
        safety = operator_block(self.network, "SafetyInvariant")
        self.assertIn("/\\ QueueEntriesUnique", safety)
        progress = operator_block(self.network, "NoPermanentQueuedFrame")
        self.assertIn("MessageQueued(s2f, kind, a)", progress)
        self.assertIn("MessageQueued(f2s, kind, a)", progress)
        self.assertIn("MessageQueued(s2d, kind, a)", progress)
        self.assertIn("ClaimQueued(a, exact)", progress)
        self.assertNotIn("Len(s2f) = 0", progress)
        self.assertNotIn("Len(f2s) = 0", progress)

    def test_default_allow_row_requires_revoked_compaction(self) -> None:
        prop = "NoStartAfterRevokedCompaction"
        self.assertIn(f"INVARIANT {prop}", DEFAULT_CFG.read_text())
        self.assertEqual(self.default_manifest["property"], prop)
        rows = [
            row
            for row in self.matrix["checks"]
            if row["id"] == "network-default-allow-after-compaction-mutant"
        ]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["property"], prop)
        block = operator_block(self.compaction, prop)
        self.assertIn('releaseCause[a] = "Revoked"', block)
        self.assertIn("startAfterRelease[a] = 0", block)


if __name__ == "__main__":
    unittest.main()
