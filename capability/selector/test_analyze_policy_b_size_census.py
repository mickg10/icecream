#!/usr/bin/env python3
"""Small deterministic gates for the raw-extent selector fit."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("analyze_policy_b_size_census.py")
SPEC = importlib.util.spec_from_file_location("analyze_policy_b_size_census", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


def row(raw: int, p29: int, grz: int) -> dict[str, int]:
    return {
        "probe_raw_bytes": raw,
        "p29_complete_bytes": p29,
        "grz_complete_bytes": grz,
    }


class PolicyBSizeCensusTest(unittest.TestCase):
    def test_threshold_minimizes_selected_bytes(self) -> None:
        rows = [
            row(100, 90, 10),
            row(200, 80, 20),
            row(500, 10, 90),
            row(600, 20, 80),
        ]
        threshold = analysis.fit_threshold(rows)
        self.assertGreater(threshold, 200)
        self.assertLess(threshold, 500)
        selected = sum(
            analysis.selected_bytes(item, item["probe_raw_bytes"] >= threshold) for item in rows
        )
        self.assertEqual(selected, 60)

    def test_tie_prefers_larger_threshold(self) -> None:
        rows = [row(100, 10, 10), row(200, 10, 10)]
        self.assertEqual(analysis.fit_threshold(rows), 201)

    def test_candidates_cover_constant_policies(self) -> None:
        self.assertEqual(analysis.threshold_candidates([row(100, 1, 2)]), [0, 101])


if __name__ == "__main__":
    unittest.main()
