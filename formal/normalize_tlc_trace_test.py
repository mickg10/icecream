#!/usr/bin/env python3
"""Self-tests for normalize_tlc_trace.py; no external packages required."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

MODULE_PATH = Path(__file__).with_name("normalize_tlc_trace.py")
SPEC = importlib.util.spec_from_file_location("normalize_tlc_trace", MODULE_PATH)
assert SPEC and SPEC.loader
normalize = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = normalize
SPEC.loader.exec_module(normalize)


class NormalizeTraceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_real_dumptrace_wrapper(self) -> None:
        document = {
            "counterexample": {
                "action": [],
                "state": [
                    [2, {"phase": {"a0": "Prepared"}}],
                    [1, {"phase": {"a0": "Absent"}}],
                ],
            },
            "vars": ["phase"],
        }
        self.assertEqual(
            normalize.normalize_json_document(document),
            [
                {"phase": {"a0": "Absent"}},
                {"phase": {"a0": "Prepared"}},
            ],
        )

    def test_text_trace_parses_functions_records_and_sequences(self) -> None:
        text = r'''
Error: Invariant ReleaseAfterRevokedConsume is violated.
Error: The behavior up to this point is:
State 1: <Initial predicate>
/\ phase = (a0 :> "Absent" @@ a1 :> "Absent")
/\ released = (a0 :> FALSE @@ a1 :> FALSE)
/\ f2s = <<>>
/\ nextF2SSeq = 1

State 2: <FReceivePrepare line 123>
/\ phase = (a0 :> "Prepared" @@ a1 :> "Absent")
/\ released = (a0 :> FALSE @@ a1 :> FALSE)
/\ f2s = <<[kind |-> "READY",
             assignment |-> a0,
             seq |-> 1]>>
/\ nextF2SSeq = 2
2 states generated, 2 distinct states found, 0 states left on queue.
'''
        self.assertEqual(
            normalize.normalize_tlc_text(text),
            [
                {
                    "phase": {"a0": "Absent", "a1": "Absent"},
                    "released": {"a0": False, "a1": False},
                    "f2s": [],
                    "nextF2SSeq": 1,
                },
                {
                    "phase": {"a0": "Prepared", "a1": "Absent"},
                    "released": {"a0": False, "a1": False},
                    "f2s": [
                        {"kind": "READY", "assignment": "a0", "seq": 1}
                    ],
                    "nextF2SSeq": 2,
                },
            ],
        )

    def test_duplicate_json_ordinals_are_rejected(self) -> None:
        document = {
            "counterexample": {
                "state": [[1, {"x": 0}], [1, {"x": 1}]]
            }
        }
        with self.assertRaisesRegex(
            normalize.TraceNormalizationError, "duplicate state ordinals"
        ):
            normalize.normalize_json_document(document)

    def test_zero_state_trace_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            normalize.TraceNormalizationError, "zero states"
        ):
            normalize.normalize_json_document([])

    def test_malformed_text_value_is_rejected(self) -> None:
        text = '''
State 1: <Initial predicate>
/\\ phase = (a0 :> "Absent"
'''
        with self.assertRaises(normalize.TraceNormalizationError):
            normalize.normalize_tlc_text(text)

    def test_cli_writes_canonical_array_and_metadata(self) -> None:
        source = self.root / "trace.json"
        output = self.root / "normalized.json"
        metadata = self.root / "metadata.json"
        source.write_text(
            json.dumps(
                {
                    "counterexample": {
                        "state": [[1, {"x": 0}], [2, {"x": 1}]]
                    }
                }
            ),
            encoding="utf-8",
        )
        self.assertEqual(
            normalize.main(
                [
                    "--input",
                    str(source),
                    "--output",
                    str(output),
                    "--metadata",
                    str(metadata),
                ]
            ),
            0,
        )
        self.assertEqual(json.loads(output.read_text()), [{"x": 0}, {"x": 1}])
        self.assertEqual(json.loads(metadata.read_text())["state_count"], 2)


if __name__ == "__main__":
    unittest.main()
