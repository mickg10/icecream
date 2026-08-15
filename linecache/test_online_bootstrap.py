#!/usr/bin/env python3

import unittest

from online_bootstrap_curves import (
    compress_frame,
    evaluate_online,
    serialize_key_batch,
    summarize_learning_gate,
)
from pretrained_superblocks import TuRegions, package_from_keys


def curve(points: int, raw: int, wire: int) -> list[dict]:
    out = []
    cumulative_raw = cumulative_wire = 0
    for tu in range(1, points + 1):
        cumulative_raw += raw
        cumulative_wire += wire
        out.append({
            "tu": tu,
            "raw_bytes": raw,
            "wire_bytes": wire,
            "cumulative_raw_bytes": cumulative_raw,
            "cumulative_charged_bytes": cumulative_wire,
            "cumulative_charged_ratio": cumulative_raw / cumulative_wire,
        })
    return out


class LearningGateTest(unittest.TestCase):
    def test_empty_curve(self) -> None:
        result = summarize_learning_gate([], 0, 0)
        self.assertIsNone(result["h200_fraction"])
        self.assertIsNone(result["c50_ratio"])

    def test_constant_250x_reaches_gate_at_64_tus(self) -> None:
        points = curve(200, 1_000, 4)
        result = summarize_learning_gate(points, 0, 200_000)
        self.assertEqual(result["c50_tu"], 100)
        self.assertEqual(result["c50_ratio"], 250.0)
        self.assertEqual(result["h200_tu"], 64)
        self.assertAlmostEqual(result["h200_fraction"], 0.32)
        self.assertEqual(result["final_window_ratio"], 250.0)

    def test_constant_100x_never_reaches_gate(self) -> None:
        points = curve(200, 1_000, 10)
        result = summarize_learning_gate(points, 0, 200_000)
        self.assertIsNone(result["h200_tu"])
        self.assertIsNone(result["h200_fraction"])

    def test_initial_package_is_charged_while_window_contains_start(self) -> None:
        points = curve(200, 1_000, 4)
        result = summarize_learning_gate(points, 100_000, 200_000)
        self.assertGreater(result["h200_tu"], 64)
        self.assertLessEqual(result["h200_fraction"], 0.50)


class FirstUseTest(unittest.TestCase):
    def test_first_use_publishes_only_an_exact_selected_phrase(self) -> None:
        common = [(100 + i, 200 + i, 1, 16) for i in range(8)]
        tus = [
            TuRegions(tu, 10_000, common + [(1_000 + tu, 2_000 + tu, 1, 16)])
            for tu in range(1, 5)
        ]
        empty = package_from_keys([], 0)
        empty_frame = compress_frame(serialize_key_batch([]), 3)
        result = evaluate_online(
            tus,
            "first-use-test",
            empty,
            empty_frame,
            3,
            (8,),
            2,
            4_096,
            4_096,
            100,
            8_192,
            "first-use",
            "ids32",
        )
        self.assertTrue(result.exact)
        self.assertGreater(result.online_promoted_assets, 0)
        self.assertGreater(result.online_published_assets, 0)
        self.assertLessEqual(
            result.online_published_assets,
            result.online_promoted_assets,
        )
        self.assertGreater(result.online_selected_tus, 0)


if __name__ == "__main__":
    unittest.main()
