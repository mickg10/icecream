#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from compare_seed_bootstrap import (
    EMPTY,
    INSTALLED,
    SEED,
    checkpoint_summary,
    compare_corpora,
    load_inputs,
)


def row(name: str, wires: list[int], raw: list[int], **extra: object) -> dict:
    cumulative_raw = 0
    curve = []
    for ordinal, (wire, raw_bytes) in enumerate(zip(wires, raw, strict=True), 1):
        cumulative_raw += raw_bytes
        curve.append(
            {
                "tu": ordinal,
                "raw_bytes": raw_bytes,
                "cumulative_raw_bytes": cumulative_raw,
                "cumulative_charged_bytes": wire,
                "cumulative_charged_ratio": cumulative_raw / wire,
            }
        )
    return {
        "name": name,
        "tus": len(curve),
        "raw_bytes": cumulative_raw,
        "charged_wire_bytes": wires[-1],
        "charged_ratio": cumulative_raw / wires[-1],
        "per_tu_curve": curve,
        "online_selected_tus": 1,
        "online_promoted_assets": 4,
        "online_published_assets": 3,
        "candidate_observations": 5,
        "final_logical_state_bytes": 100,
        "definition_wire_bytes": 7,
        "seed_assets": 2,
        "seed_model_raw_bytes": 20,
        "seed_model_compressed_bytes": 10,
        "seed_published_assets": 1,
        "exact": True,
        **extra,
    }


class SeedComparisonTest(unittest.TestCase):
    def test_prefix_comparison_and_ties_are_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.json"
            seed = root / "seed.json"
            baseline.write_text(
                json.dumps(
                    {
                        "initial_package": {"sha256": "installed"},
                        "rows": [
                            row(EMPTY, [100, 180, 260], [1_000, 1_000, 1_000]),
                            row(INSTALLED, [900, 170, 250], [1_000, 1_000, 1_000]),
                        ],
                    }
                )
            )
            seed.write_text(
                json.dumps(
                    {
                        "initial_package": {"sha256": "seed"},
                        "rows": [
                            row(
                                SEED,
                                [100, 160],
                                [1_000, 1_000],
                                pretrained_mode="seed-only",
                            )
                        ],
                    }
                )
            )
            rows, packages = load_inputs(
                (("sample", baseline), ("sample", seed))
            )
            compared = compare_corpora(rows, packages)

        self.assertEqual(len(compared), 1)
        result = compared[0]
        self.assertEqual(result["screen_tus"], 2)
        self.assertEqual(result["raw_bytes"], 2_000)
        self.assertEqual(result["seed_first_strict_win_vs_empty_tu"], 2)
        self.assertEqual(result["seed_stable_strict_win_vs_empty_tu"], 2)
        self.assertEqual(result["seed_artifact_sha256"], "seed")
        checkpoint = checkpoint_summary(rows, 1)
        self.assertEqual(checkpoint["seed_strict_wins_vs_empty"], 0)
        self.assertEqual(checkpoint["seed_ties_vs_empty"], 1)
        self.assertEqual(checkpoint["seed_strict_wins_vs_installed"], 1)

    def test_input_boundary_mismatch_is_rejected(self) -> None:
        rows = {
            "sample": {
                EMPTY: row(EMPTY, [100], [1_000]),
                INSTALLED: row(INSTALLED, [100], [999]),
                SEED: row(
                    SEED,
                    [100],
                    [1_000],
                    pretrained_mode="seed-only",
                ),
            }
        }
        with self.assertRaisesRegex(ValueError, "input boundary differs"):
            compare_corpora(
                rows,
                {
                    "sample": {
                        "sha256": "seed",
                    }
                },
            )


if __name__ == "__main__":
    unittest.main()
