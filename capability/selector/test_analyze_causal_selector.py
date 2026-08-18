#!/usr/bin/env python3
"""Boundary gates for the frozen causal selector rule."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("analyze_causal_selector.py")
SPEC = importlib.util.spec_from_file_location("analyze_causal_selector", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


def row(raw: int, trajectory: float, literal: float, root_missing: float) -> dict[str, object]:
    return {
        "probe_raw_bytes": raw,
        "p29_probe_trajectory": trajectory,
        "p29_probe_literal_wire_fraction": literal,
        "p29_probe_root_missing_wire_fraction": root_missing,
    }


class CausalSelectorTest(unittest.TestCase):
    def test_mature_large_branch(self) -> None:
        self.assertTrue(analysis.choose_p29(row(500_000_000, 0.30, 0.0, 1.0)))
        self.assertFalse(analysis.choose_p29(row(499_999_999, 0.30, 0.0, 1.0)))
        self.assertFalse(analysis.choose_p29(row(500_000_000, 0.300001, 0.0, 1.0)))

    def test_literal_shaped_branch(self) -> None:
        self.assertTrue(analysis.choose_p29(row(200_000_000, 0.30, 0.57, 0.05)))
        self.assertFalse(analysis.choose_p29(row(199_999_999, 0.30, 0.57, 0.05)))
        self.assertFalse(analysis.choose_p29(row(200_000_000, 0.30, 0.56999, 0.05)))
        self.assertFalse(analysis.choose_p29(row(200_000_000, 0.30, 0.57, 0.05001)))


if __name__ == "__main__":
    unittest.main()
