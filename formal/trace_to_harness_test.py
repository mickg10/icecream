#!/usr/bin/env python3
"""Self-tests for trace_to_harness.py; no external packages required."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("trace_to_harness.py")
SPEC = importlib.util.spec_from_file_location("trace_to_harness", MODULE_PATH)
assert SPEC and SPEC.loader
trace_to_harness = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = trace_to_harness
SPEC.loader.exec_module(trace_to_harness)


class TraceAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_json(self, name: str, value: object) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    @staticmethod
    def valid_trace() -> list[dict[str, object]]:
        return [
            {
                "phase": {"a0": "Absent"},
                "released": {"a0": False},
                "revokedConsumed": {"a0": False},
                "f2s": [],
                "lastF2SConsumed": 0,
            },
            {
                "phase": {"a0": "PrepareQueued"},
                "released": {"a0": False},
                "revokedConsumed": {"a0": False},
                "f2s": [],
                "lastF2SConsumed": 0,
            },
            {
                "phase": {"a0": "Prepared"},
                "released": {"a0": False},
                "revokedConsumed": {"a0": False},
                "f2s": [{"seq": 1, "kind": "READY"}],
                "lastF2SConsumed": 0,
            },
            {
                "phase": {"a0": "Terminal"},
                "released": {"a0": True},
                "revokedConsumed": {"a0": False},
                "f2s": [
                    {"seq": 1, "kind": "READY"},
                    {"seq": 2, "kind": "REVOKED"},
                ],
                "lastF2SConsumed": 0,
            },
        ]

    @staticmethod
    def valid_manifest() -> dict[str, object]:
        return {
            "schema": 1,
            "scenario": "release-on-enqueue-mutant",
            "property": "ReleaseAfterRevokedConsume",
            "events": [
                {
                    "name": "SPrepare.a0",
                    "all": [
                        {
                            "path": "phase.a0",
                            "from": "Absent",
                            "to": "PrepareQueued",
                        }
                    ],
                },
                {
                    "name": "FReceivePrepare.a0",
                    "all": [
                        {
                            "path": "phase.a0",
                            "from": "PrepareQueued",
                            "to": "Prepared",
                        },
                        {"path": "f2s", "len_delta": 1},
                    ],
                },
                {
                    "name": "MutantReleaseOnEnqueue.a0",
                    "all": [
                        {"path": "released.a0", "from": False, "to": True},
                        {"path": "revokedConsumed.a0", "to": False},
                    ],
                },
            ],
            "required_subsequence": [
                "SPrepare.a0",
                "FReceivePrepare.a0",
                "MutantReleaseOnEnqueue.a0",
            ],
            "final_all": [
                {"path": "released.a0", "eq": True},
                {"path": "revokedConsumed.a0", "eq": False},
            ],
            "harness_steps": [
                {
                    "after": "FReceivePrepare.a0",
                    "emit": "barrier_after_worker_reservation",
                }
            ],
        }

    def test_valid_trace_emits_barrier(self) -> None:
        trace = self.write_json("trace.json", self.valid_trace())
        manifest = self.write_json("manifest.json", self.valid_manifest())
        result = trace_to_harness.validate_trace(trace, manifest)
        self.assertEqual(
            result["event_sequence"],
            [
                "SPrepare.a0",
                "FReceivePrepare.a0",
                "MutantReleaseOnEnqueue.a0",
            ],
        )
        self.assertEqual(result["harness_steps"][0]["event_transition_index"], 2)
        self.assertEqual(result["trace"]["state_count"], 4)

    def test_zero_state_trace_is_rejected(self) -> None:
        trace = self.write_json("trace.json", [])
        manifest = self.write_json("manifest.json", self.valid_manifest())
        with self.assertRaisesRegex(
            trace_to_harness.TraceValidationError, "zero states"
        ):
            trace_to_harness.validate_trace(trace, manifest)

    def test_wrong_event_order_is_rejected(self) -> None:
        states = self.valid_trace()
        states[1], states[2] = states[2], states[1]
        trace = self.write_json("trace.json", states)
        manifest = self.write_json("manifest.json", self.valid_manifest())
        with self.assertRaises(trace_to_harness.TraceValidationError):
            trace_to_harness.validate_trace(trace, manifest)

    def test_ambiguous_event_is_rejected_by_default(self) -> None:
        manifest = self.valid_manifest()
        manifest["events"].append(
            {
                "name": "AlsoPrepare.a0",
                "all": [
                    {
                        "path": "phase.a0",
                        "from": "Absent",
                        "to": "PrepareQueued",
                    }
                ],
            }
        )
        trace = self.write_json("trace.json", self.valid_trace())
        manifest_path = self.write_json("manifest.json", manifest)
        with self.assertRaisesRegex(
            trace_to_harness.TraceValidationError, "ambiguous"
        ):
            trace_to_harness.validate_trace(trace, manifest_path)

    def test_final_predicate_path_typo_is_rejected(self) -> None:
        manifest = self.valid_manifest()
        manifest["final_all"] = [{"path": "released.typo", "eq": True}]
        trace = self.write_json("trace.json", self.valid_trace())
        manifest_path = self.write_json("manifest.json", manifest)
        with self.assertRaisesRegex(trace_to_harness.TraceValidationError, "absent"):
            trace_to_harness.validate_trace(trace, manifest_path)

    def test_lasso_requires_retained_log_marker(self) -> None:
        manifest = self.valid_manifest()
        manifest["require_lasso"] = True
        trace = self.write_json("trace.json", self.valid_trace())
        manifest_path = self.write_json("manifest.json", manifest)
        no_lasso = self.root / "no-lasso.log"
        no_lasso.write_text("Model checking completed\n", encoding="utf-8")
        with self.assertRaisesRegex(
            trace_to_harness.TraceValidationError, "requires a lasso"
        ):
            trace_to_harness.validate_trace(
                trace, manifest_path, tlc_log_path=no_lasso
            )

        lasso = self.root / "lasso.log"
        lasso.write_text(
            "State 9: <Action line 1>\nState 10: Back to state 4\n",
            encoding="utf-8",
        )
        result = trace_to_harness.validate_trace(
            trace, manifest_path, tlc_log_path=lasso
        )
        self.assertEqual(
            result["lasso"],
            {"kind": "back-edge", "target_state_ordinal": 4},
        )

    def test_json_pointer_and_numeric_delta(self) -> None:
        trace = [
            {"lastF2SConsumed": 0, "nested": {"items": [1]}},
            {"lastF2SConsumed": 2, "nested": {"items": [1, 2]}},
        ]
        manifest = {
            "schema": 1,
            "scenario": "fifo-bypass",
            "events": [
                {
                    "name": "F2SBypass",
                    "all": [
                        {"path": "/lastF2SConsumed", "delta_gt": 1},
                        {"path": "nested.items", "len_delta": 1},
                    ],
                }
            ],
            "required_subsequence": ["F2SBypass"],
        }
        trace_path = self.write_json("trace.json", trace)
        manifest_path = self.write_json("manifest.json", manifest)
        result = trace_to_harness.validate_trace(trace_path, manifest_path)
        self.assertEqual(result["event_sequence"], ["F2SBypass"])

    def test_check_all_uses_relative_trace_and_log(self) -> None:
        case = self.root / "case"
        case.mkdir()
        self.write_json("case/counterexample.json", self.valid_trace())
        (case / "tlc.log").write_text(
            "State 4: Back to state 2\n", encoding="utf-8"
        )
        manifest = self.valid_manifest()
        manifest["trace"] = "counterexample.json"
        manifest["tlc_log"] = "tlc.log"
        manifest["require_lasso"] = True
        self.write_json("case/release.manifest.json", manifest)
        results = trace_to_harness._check_all(self.root)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["lasso"]["target_state_ordinal"], 2)


if __name__ == "__main__":
    unittest.main()
