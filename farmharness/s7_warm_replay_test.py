"""Focused contract tests for the explicit-live S7 adapter."""

import tempfile
import unittest
from pathlib import Path

import s7_warm_replay as replay


class ExplicitReplayContractTest(unittest.TestCase):
    def test_matrix_contains_every_profile_corpus_and_regime(self):
        self.assertEqual(len(replay.SUPPORTED_CELLS), 32)
        for profile in replay.SUPPORTED_PROFILES:
            for corpus in ("fmt", "RocksDB", "DuckDB", "LLVM-1238"):
                for regime in ("cold", "warm"):
                    self.assertIn(f"{corpus}/{profile}/{regime}", replay.SUPPORTED_CELLS)

    def test_symlink_input_is_held_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target.ii"
            target.write_bytes(b"live")
            link = root / "input.ii"
            link.symlink_to(target)
            with self.assertRaises(replay.Hold):
                replay.read_input(link, "measured input")

    def test_profile_environment_keeps_grz_name_at_s7_boundary(self):
        self.assertEqual(replay.PROFILE_ENV["GRZ_RESIDUAL"], "GRZ")
        self.assertEqual(replay.schema_for_cell("RocksDB/P29/cold"),
                         "icecream-s7-rocksdb-p29-cold-conformance-v2")
        self.assertEqual(replay.schema_for_cell("DuckDB/ZSTD_TU/cold"),
                         "icecream-s7-duckdb-zstd-tu-cold-conformance-v2")


if __name__ == "__main__":
    unittest.main()
