#!/usr/bin/env python3
"""Small seam tests for the S5 paired runner (no network or product daemon)."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from farmharness.s5_paired_build import (
    _remote_script, immutable_json, load_workload, preflight, role_manifest, run_build,
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
        self.assertEqual(cache.count('echo "S5_MEASURE_START_NS='), 2)

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


if __name__ == "__main__":
    unittest.main()
