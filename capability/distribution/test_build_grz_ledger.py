#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("build_grz_ledger.py")
SPEC = importlib.util.spec_from_file_location("build_grz_ledger", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = builder
SPEC.loader.exec_module(builder)


class GrzLedgerBuilderTest(unittest.TestCase):
    def test_prefix_points_cover_every_small_tu_and_large_build_boundaries(self) -> None:
        small = [SimpleNamespace(build=0) for _ in range(4)]
        self.assertEqual(builder.prefix_points(4, 64, small), [1, 2, 3, 4])
        large = [SimpleNamespace(build=0) for _ in range(100)] + [
            SimpleNamespace(build=1) for _ in range(100)
        ]
        self.assertEqual(
            builder.prefix_points(200, 64, large), [1, 64, 100, 128, 192, 200]
        )

    def test_streaming_prefix_copy_and_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            prefix = root / "prefix"
            source.write_bytes(bytes(range(251)) * 100)
            builder.copy_prefix(source, prefix, 12_345)
            self.assertTrue(builder.file_equals_prefix(prefix, source, 12_345))
            prefix.write_bytes(prefix.read_bytes()[:-1] + b"x")
            self.assertFalse(builder.file_equals_prefix(prefix, source, 12_345))


if __name__ == "__main__":
    unittest.main()
