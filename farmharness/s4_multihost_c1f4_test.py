#!/usr/bin/env python3
"""Focused local tests for the physical S4 contention runner."""

from __future__ import annotations

import unittest

from farmharness import s4_multihost_c1f4 as runner


class FourHostRunnerTests(unittest.TestCase):
    def test_active_s5_detection_is_specific(self) -> None:
        process_text = """\
10 python3 -u farmharness/s5_paired_build.py --warm-blocks 1
11 python3 farmharness/s4_multihost_c1f4.py --p50-root /tmp/x
12 python3 unrelated.py
"""
        self.assertEqual(
            runner.active_s5_processes(process_text),
            ["10 python3 -u farmharness/s5_paired_build.py --warm-blocks 1"],
        )

    def test_parse_fields_keeps_last_value(self) -> None:
        fields = runner.parse_fields(
            "noise\nS4_CACHE_OBSERVED=0\nS4_CACHE_OBSERVED=1\n"
            "S4_STATUS=PASS reason=ok\n"
        )
        self.assertEqual(fields["S4_CACHE_OBSERVED"], "1")
        self.assertEqual(fields["S4_STATUS"], "PASS reason=ok")

    def test_four_host_pass_requires_every_worker_cache_log(self) -> None:
        fields = {
            "S4_STATUS": "PASS reason=four-physical-workers-remote-byte-identical",
            "S4_REMOTE_COMPILE": "4",
            "S4_BYTE_IDENTICAL": "4",
            "S4_WORKER_SELECTION": ",".join(runner.WORKER_NAMES.values()),
            "S4_CACHE_OBSERVED": "1",
            "S4_LEGACY_OBSERVED": "0",
        }
        logs = {host: "P50 CompileFile attached exact ZSTD_TU input" for host in runner.HOSTS}
        self.assertEqual(runner.classify_result(0, fields, logs)[0], "PASS")
        logs["research7"] = "ordinary compile completed"
        self.assertEqual(
            runner.classify_result(0, fields, logs),
            ("HOLD", "cache-path-not-observed-on-every-physical-worker"),
        )

    def test_legacy_observation_is_product_failure(self) -> None:
        fields = {
            "S4_STATUS": "PASS reason=four-physical-workers-remote-byte-identical",
            "S4_REMOTE_COMPILE": "4",
            "S4_BYTE_IDENTICAL": "4",
            "S4_WORKER_SELECTION": ",".join(runner.WORKER_NAMES.values()),
            "S4_CACHE_OBSERVED": "1",
            "S4_LEGACY_OBSERVED": "0",
        }
        logs = {host: "ZSTD_TU" for host in runner.HOSTS}
        logs["q2"] += "\nfallback_local"
        self.assertEqual(
            runner.classify_result(0, fields, logs),
            ("FAIL", "legacy-path-observed-on-physical-worker"),
        )

    def test_client_hold_is_not_promoted(self) -> None:
        self.assertEqual(
            runner.classify_result(77, {"S4_STATUS": "HOLD reason=no-worker"}, {}),
            ("HOLD", "no-worker"),
        )

    def test_two_host_pass_uses_only_selected_workers(self) -> None:
        names = [runner.WORKER_NAMES["q3"], runner.WORKER_NAMES["research6"]]
        fields = {
            "S4_STATUS": "PASS reason=physical-workers-remote-byte-identical",
            "S4_REMOTE_COMPILE": "2",
            "S4_BYTE_IDENTICAL": "2",
            "S4_WORKER_SELECTION": ",".join(names),
            "S4_CACHE_OBSERVED": "1",
            "S4_LEGACY_OBSERVED": "0",
        }
        logs = {
            "q3": "P50 CompileFile attached exact ZSTD_TU input",
            "research6": "P50 CompileFile attached exact ZSTD_TU input",
        }
        self.assertEqual(
            runner.classify_result(0, fields, logs, names),
            ("PASS", "2-physical-workers-remote-byte-identical"),
        )
        fields["S4_REMOTE_COMPILE"] = "4"
        self.assertEqual(
            runner.classify_result(0, fields, logs, names),
            ("FAIL", "2-compile-byte-ledger-incomplete"),
        )

    def test_client_script_runs_final_compiles_concurrently(self) -> None:
        self.assertIn('pids+=("$!")', runner.CLIENT_SCRIPT)
        self.assertIn('wait "${pids[$((i-1))]}"', runner.CLIENT_SCRIPT)
        self.assertIn('[ "$load" = same ]', runner.CLIENT_SCRIPT)
        self.assertIn('4) values_count=16384', runner.CLIENT_SCRIPT)
        self.assertIn('for i in $(seq 1 "$worker_count")', runner.CLIENT_SCRIPT)


if __name__ == "__main__":
    unittest.main()
