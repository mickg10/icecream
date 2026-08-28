#!/usr/bin/env python3
"""Focused completeness tests for the P50 runtime evidence collector."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("p50_runtime_evidence.py")
SPEC = importlib.util.spec_from_file_location("p50_runtime_evidence", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
EVIDENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVIDENCE)


def ready_line(*, attempt: int = 1, f_store_generation: int = 3,
               pid: int = 4, identity: int = 1) -> str:
    return (
        f"READY v2 generation=1 attempt={attempt} "
        f"F_STORE_GENERATION={f_store_generation} "
        f"DERIVATION_VERSION=1 pid={pid} "
        f"C_STORE_GUID={identity:032x} F_STORE_GUID={identity + 100:032x} "
        f"PATH=/tmp/p50-cache-{identity}.sock DIGEST={identity + 200:032x} "
        f"DEV=5 INO={identity + 300}\n"
    )


def identity_line(*, job_id: int = 7, tu_seq: int = 8) -> str:
    return (
        '{"record":"compile-result-identity","job_id":%d,'
        '"assignment_epoch":9,"assignment_nonce":10,"c_guid":11,'
        '"tu_seq":%d}\n' % (job_id, tu_seq)
    )


class RuntimeEvidenceTest(unittest.TestCase):
    def collect(self, identity: str | None, ready_text: str | None = None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ready_path = root / "ready.trace"
            lifecycle = root / "lifecycle.trace"
            worker = root / "worker.log"
            trace = root / "compile-identity.jsonl"
            ready_path.write_text(
                ready_text if ready_text is not None else ready_line(),
                encoding="utf-8")
            lifecycle.write_text("P50_LIFECYCLE action=2 status=0\n", encoding="utf-8")
            worker.write_text("P50 input settlement job 7 action 2\n", encoding="utf-8")
            if identity is not None:
                trace.write_text(identity, encoding="utf-8")
            return EVIDENCE.collect(SimpleNamespace(
                experiment_id="focused", run_id="run-1", ready=ready_path,
                lifecycle=lifecycle, worker_log=worker,
                identity_trace=trace, output=root / "runtime.json",
            ))

    def test_unique_valid_rows_make_only_identity_pass(self) -> None:
        document = self.collect(identity_line() + identity_line(job_id=12, tu_seq=13))
        self.assertEqual(
            {row["field"]: row["status"] for row in document["identity_status"]},
            {"c_guid": "PASS", "tu_seq": "PASS"},
        )
        verification = EVIDENCE.verify_runtime_artifact(document)
        self.assertEqual(verification["status"], "HOLD")
        self.assertEqual(verification["issues"], ["statistics_document_missing"])

    def test_missing_trace_holds_identity(self) -> None:
        document = self.collect(None)
        self.assertTrue(all(row["status"] == "HOLD"
                            for row in document["identity_status"]))
        self.assertIn("compile_identity_missing",
                      EVIDENCE.verify_runtime_artifact(document)["issues"])

    def test_malformed_trace_fails_closed(self) -> None:
        document = self.collect("not-json\n")
        self.assertEqual(document["runtime"]["compile_identity"],
                         [{"record": "invalid"}])
        self.assertIn("compile_identity_row_invalid",
                      EVIDENCE.verify_runtime_artifact(document)["issues"])

    def test_duplicate_trace_holds_identity(self) -> None:
        line = identity_line()
        document = self.collect(line + line)
        self.assertTrue(all(row["status"] == "HOLD"
                            for row in document["identity_status"]))
        self.assertIn("compile_identity_duplicate",
                      EVIDENCE.verify_runtime_artifact(document)["issues"])

    def test_extra_or_wrongly_typed_fields_are_invalid(self) -> None:
        wrong_type = identity_line().replace('"job_id":7', '"job_id":true')
        document = self.collect(wrong_type)
        self.assertIn("compile_identity_row_invalid",
                      EVIDENCE.verify_runtime_artifact(document)["issues"])

    def test_monotonic_ready_replacement_chain_is_not_ambiguous(self) -> None:
        ready = (ready_line(attempt=1, f_store_generation=1, pid=4, identity=1) +
                 ready_line(attempt=2, f_store_generation=2, pid=5, identity=2) +
                 ready_line(attempt=3, f_store_generation=3, pid=6, identity=3))
        document = self.collect(identity_line(), ready)
        self.assertNotIn("f_store_generation_ambiguous",
                         EVIDENCE.verify_runtime_artifact(document)["issues"])

    def test_repeated_ready_generation_is_ambiguous(self) -> None:
        ready = (ready_line(attempt=1, f_store_generation=1, pid=4, identity=1) +
                 ready_line(attempt=2, f_store_generation=1, pid=5, identity=2))
        document = self.collect(identity_line(), ready)
        self.assertIn("f_store_generation_ambiguous",
                      EVIDENCE.verify_runtime_artifact(document)["issues"])


if __name__ == "__main__":
    unittest.main()
