#!/usr/bin/env python3

from __future__ import annotations

import unittest

from compare_seed_bootstrap import EMPTY, INSTALLED, SEED
from pre_shared_transfer_matrix import (
    aggregate_checkpoint,
    make_long_rows,
    render_markdown,
    transfer_point,
    validate_rows,
)


def make_row(name: str, wires: list[int], model_bytes: int = 0) -> dict:
    curve = []
    cumulative_raw = 0
    for ordinal, charged in enumerate(wires, 1):
        cumulative_raw += 1_000
        curve.append(
            {
                "tu": ordinal,
                "raw_bytes": 1_000,
                "cumulative_raw_bytes": cumulative_raw,
                "cumulative_charged_bytes": charged,
            }
        )
    return {
        "name": name,
        "tus": len(curve),
        "raw_bytes": cumulative_raw,
        "initial_model_wire_bytes": model_bytes,
        "per_tu_curve": curve,
        "exact": True,
    }


def corpus_rows() -> dict[str, dict[str, dict]]:
    return {
        "sample": {
            EMPTY: make_row(EMPTY, [100, 200, 300]),
            INSTALLED: make_row(INSTALLED, [150, 230, 310], model_bytes=50),
            SEED: make_row(SEED, [100, 180, 270]),
        }
    }


class TransferPointTest(unittest.TestCase):
    def test_installed_model_is_excluded_from_build_wire(self) -> None:
        point = transfer_point(corpus_rows()["sample"][INSTALLED], 2)
        self.assertEqual(point["build_wire_bytes"], 180)
        self.assertEqual(point["installation_bytes_excluded"], 50)
        self.assertAlmostEqual(point["ratio"], 2_000 / 180)

    def test_long_rows_include_requested_and_full_boundaries(self) -> None:
        rows = corpus_rows()
        validate_rows(rows)
        output = make_long_rows(rows, (2,))
        self.assertEqual(len(output), 6)
        self.assertEqual({row["checkpoint"] for row in output}, {"2", "full"})
        seed = next(
            row
            for row in output
            if row["checkpoint"] == "2" and row["method"] == "pre_shared_c_only"
        )
        self.assertEqual(seed["saved_vs_empty_bytes"], 20)
        self.assertAlmostEqual(seed["saved_vs_empty_percent"], 10.0)

    def test_aggregate_uses_total_bytes_and_names_lower_fixed_method(self) -> None:
        result = aggregate_checkpoint(corpus_rows(), 2)
        self.assertEqual(result["raw_bytes"], 2_000)
        self.assertEqual(
            result["methods"]["pre_shared_installed"]["build_wire_bytes"], 180
        )
        self.assertEqual(
            result["methods"]["pre_shared_c_only"]["build_wire_bytes"], 180
        )
        self.assertEqual(result["lower_fixed_pre_shared_method"], "pre_shared_installed")

    def test_boundary_mismatch_is_rejected(self) -> None:
        rows = corpus_rows()
        rows["sample"][SEED]["per_tu_curve"][1]["raw_bytes"] = 999
        with self.assertRaisesRegex(ValueError, "input boundary differs"):
            validate_rows(rows)

    def test_markdown_states_installation_exclusion(self) -> None:
        rows = corpus_rows()
        long_rows = make_long_rows(rows, (2,))
        aggregates = [aggregate_checkpoint(rows, 2), aggregate_checkpoint(rows, None)]
        report = render_markdown(
            {
                "aggregates": aggregates,
                "checkpoints": [2],
                "rows": long_rows,
                "packages": [
                    {
                        "corpus": "sample",
                        "sha256": "abc",
                        "raw_bytes": 10,
                        "compressed_bytes": 5,
                    }
                ],
            }
        )
        self.assertIn("not build traffic", report)
        self.assertIn("cumulative_charged_bytes - initial_model_wire_bytes", report)
        self.assertIn("Region-program structural layer only", report)


if __name__ == "__main__":
    unittest.main()
