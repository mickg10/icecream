#!/usr/bin/env python3
"""Self-tests for tlc_text_trace.py; no external packages required."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


sys.dont_write_bytecode = True
MODULE_PATH = Path(__file__).with_name("tlc_text_trace.py")
FIXTURE_PATH = (
    Path(__file__).with_name("testdata")
    / "core-mixed-token-mutant-1.7.4.log"
)
SPEC = importlib.util.spec_from_file_location("tlc_text_trace", MODULE_PATH)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)


class ValueParserTests(unittest.TestCase):
    def test_scalars_sequences_sets_and_maps(self) -> None:
        self.assertEqual(module.parse_tla_value("TRUE"), True)
        self.assertEqual(module.parse_tla_value("-17"), -17)
        self.assertEqual(module.parse_tla_value('"a\\nb"'), "a\nb")
        self.assertEqual(
            module.parse_tla_value("<<1, FALSE, \"x\">>"),
            [1, False, "x"],
        )
        self.assertEqual(module.parse_tla_value("{3, 1}"), [3, 1])
        self.assertEqual(
            module.parse_tla_value(
                '["a0" |-> "Prepared", "a1" |-> "Absent"]'
            ),
            {"a0": "Prepared", "a1": "Absent"},
        )
        self.assertEqual(
            module.parse_tla_value('( "a0" :> 1 @@ "a1" :> 2 )'),
            {"a0": 1, "a1": 2},
        )

    def test_full_text_counterexample_and_lasso(self) -> None:
        text = r'''
TLC2 Version 2.19 of 08 August 2024 (rev: 5a47802)
Error: Invariant ReleaseSafety is violated.
Error: The behavior up to this point is:
State 1: <Initial predicate>
/\ phase = ["a0" |-> "Absent", "a1" |-> "Absent"]
/\ released = ["a0" |-> FALSE, "a1" |-> FALSE]
/\ f2s = <<>>
/\ allocated = {}
State 2: <Prepare line 1, col 1 to line 2, col 1>
/\ phase = ["a0" |-> "Prepared", "a1" |-> "Absent"]
/\ released = ["a0" |-> FALSE, "a1" |-> FALSE]
/\ f2s = <<[kind |-> "READY",
             assignment |-> "a0", seq |-> 1]>>
/\ allocated = {1}
State 3: Back to state 2
3 states generated, 2 distinct states found, 0 states left on queue.
'''
        parsed = module.parse_tlc_text_trace(text)
        self.assertEqual(parsed.state_ordinals, [1, 2])
        self.assertEqual(parsed.states[1]["phase"]["a0"], "Prepared")
        self.assertEqual(parsed.states[1]["f2s"][0]["seq"], 1)
        self.assertEqual(parsed.states[1]["allocated"], [1])
        self.assertEqual(
            parsed.lasso,
            {
                "kind": "back-edge",
                "source_state_ordinal": 3,
                "target_state_ordinal": 2,
            },
        )

    def test_retained_mixed_token_footer_is_not_part_of_last_value(self) -> None:
        text = FIXTURE_PATH.read_text(encoding="utf-8")
        parsed = module.parse_tlc_text_trace(text)
        self.assertEqual(parsed.state_ordinals, [1, 2, 3])
        self.assertEqual(len(parsed.states), 3)
        self.assertEqual(
            parsed.states[2]["terminalCount"],
            {"a0": 0, "a1": 0},
        )
        self.assertEqual(parsed.states[2]["phase"]["a0"], "Claimed")
        self.assertEqual(parsed.states[2]["claimant"]["a0"], "a1")

    def test_incomplete_value_before_footer_is_rejected(self) -> None:
        text = '''
State 1: <Initial>
/\\ x = 0
State 2: <Next>
/\\ x = [a0 |-> 0
2 states generated, 2 distinct states found, 0 states left on queue.
'''
        with self.assertRaisesRegex(
            module.TLCTextTraceError,
            "incomplete.*top-level line",
        ):
            module.parse_tlc_text_trace(text)

    def test_duplicate_variable_is_rejected(self) -> None:
        text = '''
State 1: <Initial>
/\\ x = 1
/\\ x = 2
State 2: <Next>
/\\ x = 3
'''
        with self.assertRaisesRegex(
            module.TLCTextTraceError, "duplicate variable"
        ):
            module.parse_tlc_text_trace(text)

    def test_trailing_value_input_is_rejected(self) -> None:
        with self.assertRaisesRegex(module.TLCTextTraceError, "trailing input"):
            module.parse_tla_value("TRUE FALSE")

    def test_zero_and_one_state_are_rejected(self) -> None:
        with self.assertRaisesRegex(module.TLCTextTraceError, "no TLC"):
            module.parse_tlc_text_trace("No behavior here")
        with self.assertRaisesRegex(module.TLCTextTraceError, "fewer than two"):
            module.parse_tlc_text_trace("State 1: <Initial>\n/\\ x = 1\n")


if __name__ == "__main__":
    unittest.main()
