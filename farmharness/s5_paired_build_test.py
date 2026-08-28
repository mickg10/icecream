#!/usr/bin/env python3
"""Small seam tests for the S5 paired runner (no network or product daemon)."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from farmharness.s5_paired_build import (
    _parse_tu_ledger, _remote_script, immutable_json, load_workload, preflight,
    role_manifest, run_build,
    schedule, schedule_matrix,
)


class PairedRunnerTest(unittest.TestCase):
    def test_counterbalanced_schedule(self):
        rows = schedule(4)
        self.assertEqual([row["order"] for row in rows], ["AB", "BA", "AB", "BA"])
        self.assertEqual([row["regime"] for row in rows], ["cold"] * 4)

    def test_preregistered_matrix_has_private_balanced_regimes(self):
        rows = schedule_matrix()
        self.assertEqual(len(rows), 8)
        self.assertEqual([row["regime"] for row in rows[:4]], ["cold"] * 4)
        self.assertEqual([row["regime"] for row in rows[4:]], ["warm"] * 4)
        self.assertEqual([row["order"] for row in rows], ["AB", "BA"] * 4)

    def test_manifest_is_append_only_and_mode_0444(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            digest = immutable_json(path, {"b": 2, "a": 1})
            self.assertTrue(digest)
            self.assertEqual(path.stat().st_mode & 0o777, 0o444)
            with self.assertRaises(FileExistsError):
                immutable_json(path, {"a": 99})

    def test_role_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "exact-role-binary-mismatch"):
                role_manifest(Path(directory))

    def test_unrelated_local_processes_are_recorded_but_do_not_veto_clean_target(self):
        args = type("Args", (), {"timeout": 1.0})()
        with mock.patch("farmharness.s5_paired_build._local_process_facts",
                        return_value=[{"pid": "123", "command": "iceccd"}]), \
             mock.patch("farmharness.s5_paired_build._target_preflight",
                        return_value={"status": "READY", "reason": "target-clean"}) as target:
            result = preflight(args)
        self.assertEqual(result["status"], "READY")
        self.assertEqual(result["reason"], "target-uncontaminated")
        self.assertEqual(result["local_processes"][0]["pid"], "123")
        self.assertEqual(result["target"]["reason"], "target-clean")
        target.assert_called_once_with(args)

    def test_warm_cell_is_hold_without_remote_execution(self):
        block = {"block_id": "warm-01", "regime": "warm", "order": "AB"}
        tu = {"tu_id": "fmt-0001-format", "source": "src/format.cc",
              "source_sha256": "a" * 64}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch("farmharness.s5_paired_build.subprocess.run") as remote:
            args = type("Args", (), {})()
            row = run_build(args, Path(directory), block, "cache", tu, "archive")
        self.assertEqual(row["status"], "HOLD")
        self.assertEqual(row["reason"], "warm-state-cell-not-available")
        self.assertIsNone(row["start_ns"])
        remote.assert_not_called()

    def test_remote_script_scopes_strict_assignment_and_measures_remote_compile(self):
        tu = {"source": "src/format.cc", "flags": ["-O3"]}
        cache = _remote_script("archive", [tu], "cache", Path("/unused"))
        legacy = _remote_script("archive", [tu], "legacy", Path("/unused"))
        self.assertIn("--assignment-fence-mode strict-nonce", cache)
        self.assertNotIn("--assignment-fence-mode strict-nonce", legacy)
        self.assertIn("S5_MEASURE_END_NS=$(date +%s%N)\ng++ -O3", cache)
        # The marker is emitted by each S4 result cell; all lifecycle result
        # cells carry the same aggregate timing boundary.
        self.assertEqual(cache.count('echo "S5_MEASURE_START_NS='), 3)

    def test_manifest_deduplicates_same_translation_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            source = root / "src" / "format.cc"
            source.write_text("int fmt_smoke() { return 0; }\n", encoding="utf-8")
            database = root / "compile_commands.json"
            row = {"directory": str(root), "command": f"g++ -I{root}/include -c {source} -o x.o", "file": str(source)}
            database.write_text(json.dumps([row, row]), encoding="utf-8")
            result = load_workload(database, root, ["src/format.cc"])
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0]["source"], "src/format.cc")
            self.assertEqual(result[0]["source_sha256"],
                             "643617e36915cf85055f989c78c0b41d7709c4f91ee457de95f99c5efb921764")

    def test_full_manifest_is_one_remote_lifecycle_with_aggregate_ledger(self):
        tus = [
            {"tu_id": "fmt-0001-a", "source": "src/a.cc", "flags": ["-O2"]},
            {"tu_id": "fmt-0002-b", "source": "src/b.cc", "flags": ["-O3"]},
        ]
        script = _remote_script("archive", tus, "cache", Path("/unused"))
        self.assertEqual(script.count("WORK=$(mktemp -d"), 1)
        self.assertEqual(script.count("# S5 measured TU"), 2)
        self.assertEqual(script.count("S5_TU_LEDGER"), 2)
        self.assertIn("S5_MEASURE_START_NS=$(date +%s%N)", script)
        self.assertIn("S5_MEASURE_END_NS=$(date +%s%N)", script)

    def test_warm_script_prewarms_before_timing_and_binds_product_identity(self):
        tus = [{"tu_id": "fmt-0001-a", "source": "src/a.cc", "flags": ["-O2"]}]
        script = _remote_script("archive", tus, "cache", Path("/unused"), warm=True)
        self.assertIn("# S5 prewarm: all TUs", script)
        self.assertLess(script.index("S5 prewarm TU"),
                        script.index("S5_MEASURE_START_NS=$(date +%s%N)"))
        self.assertIn("S5_PREWARM_STATE_DIGEST", script)
        self.assertIn("C_STORE_GUID", script)
        self.assertIn("F_STORE_GENERATION", script)
        self.assertIn("TU_SEQ", script)
        self.assertIn('WARM=${8:-0}', script)

    def test_parse_exact_tu_ledger(self):
        stdout = ("S5_TU_LEDGER tu_id=fmt-a source=src/a.cc "
                   "remote_sha256=" + "a" * 64 + " remote_bytes=12 "
                   "reference_sha256=" + "b" * 64 + " reference_bytes=12 byte_identical=1\n")
        rows = _parse_tu_ledger(stdout)
        self.assertEqual(rows[0]["tu_id"], "fmt-a")
        self.assertEqual(rows[0]["remote_bytes"], 12)
        self.assertTrue(rows[0]["byte_identical"])

    def test_build_row_keeps_one_aggregate_and_exact_ledger(self):
        tus = [{"tu_id": "fmt-a", "source": "src/a.cc", "source_sha256": "a" * 64,
                "flags": ["-O2"]},
               {"tu_id": "fmt-b", "source": "src/b.cc", "source_sha256": "b" * 64,
                "flags": ["-O2"]}]
        ledger = "".join(
            f"S5_TU_LEDGER tu_id=fmt-{name} source=src/{name}.cc "
            f"remote_sha256={'c' * 64} remote_bytes=8 "
            f"reference_sha256={'c' * 64} reference_bytes=8 byte_identical=1\n"
            for name in ("a", "b"))
        stdout = ("S4_STATUS=PASS reason=remote-byte-identical\n"
                  "S5_MEASURE_START_NS=100\nS5_MEASURE_END_NS=2100\n"
                  "S4_CACHE_OBSERVED=1\nS4_LEGACY_OBSERVED=0\n"
                  "S4_REMOTE_COMPILE=1\nS4_BYTE_IDENTICAL=1\n" + ledger)
        args = type("Args", (), {"p50_remote_root": "/tmp/staged",
                                  "source_root": Path("/unused"), "timeout": 2.0})()
        block = {"block_id": "cold-01", "regime": "cold", "order": "AB"}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch("farmharness.s5_paired_build.subprocess.run",
                         return_value=subprocess.CompletedProcess([], 0, stdout, "")):
            row = run_build(args, Path(directory), block, "cache", tus, "archive")
        self.assertEqual(row["status"], "PASS")
        self.assertEqual(row["tu_count"], 2)
        self.assertEqual(len(row["tu_ledger"]), 2)
        self.assertAlmostEqual(row["aggregate_wall_seconds"], 0.000002)
        self.assertEqual(row["command"][-3:], ["-", "c1f1", "0"])


if __name__ == "__main__":
    unittest.main()
