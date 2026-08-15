#!/usr/bin/env python3

import unittest

from online_bootstrap_curves import (
    RegionIdStore,
    canonical_atom_frame,
    compress_frame,
    context_run_lengths,
    coverage_materialization_keys,
    decode_canonical_atom_frame,
    deserialize_context_runs,
    evaluate_online,
    scoped_materialization_keys,
    scoped_phrase_candidates,
    serialize_context_runs,
    serialize_key_batch,
    summarize_learning_gate,
    tu_with_context_runs,
)
from pretrained_superblocks import (
    ExactEncoder,
    TuRegions,
    encode_regions,
    decode_key,
    expected_regions,
    package_from_keys,
    put_varint,
)


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
    def test_context_run_sidecar_reconstructs_exact_boundaries(self) -> None:
        tu = TuRegions(
            1,
            10_000,
            [
                (100 + i, 200 + i, context, 16)
                for i, context in enumerate((4, 4, 7, 9, 9, 9))
            ],
        )
        lengths = context_run_lengths(tu)
        self.assertEqual(lengths, [2, 1, 3])
        recovered_lengths = deserialize_context_runs(
            serialize_context_runs(lengths),
            len(tu.regions),
        )
        reconstructed = tu_with_context_runs(
            expected_regions(tu),
            recovered_lengths,
        )
        self.assertEqual(context_run_lengths(reconstructed), lengths)

    def test_canonical_atom_state_is_rebuilt_from_exact_regions(self) -> None:
        regions = [(10 + i, 20 + i, 16) for i in range(4)]
        phrase = encode_regions(regions[:2])
        package = package_from_keys([phrase], len(phrase))
        sender = RegionIdStore()
        receiver = RegionIdStore()

        first_keys = [phrase, *(encode_regions((region,)) for region in regions[2:])]
        first_frame = canonical_atom_frame(first_keys, package, sender)
        recovered = decode_canonical_atom_frame(first_frame, package, receiver)
        self.assertEqual(recovered, regions)
        sender.observe(regions)
        receiver.observe(recovered)
        self.assertEqual(receiver.ids, sender.ids)

        second_keys = [encode_regions((region,)) for region in reversed(regions)]
        second_frame = canonical_atom_frame(second_keys, package, sender)
        self.assertEqual(
            decode_canonical_atom_frame(second_frame, package, receiver),
            list(reversed(regions)),
        )

        duplicate = encode_regions(((999, 1000, 8),))
        malformed = bytearray(put_varint(2))
        for dynamic_id in (len(receiver.values), len(receiver.values) + 1):
            malformed.append(ExactEncoder.RAW_DEFINE)
            malformed += put_varint(dynamic_id)
            malformed += put_varint(len(duplicate))
            malformed += duplicate
        with self.assertRaises(ValueError):
            decode_canonical_atom_frame(bytes(malformed), package, receiver)

    def test_cross_context_phrase_is_generated_and_matched_exactly(self) -> None:
        tu = TuRegions(
            1,
            10_000,
            [
                (100 + i, 200 + i, 1 if i < 4 else 2, 16)
                for i in range(8)
            ],
        )
        cross_context = encode_regions(expected_regions(tu))
        self.assertNotIn(
            cross_context,
            set(scoped_phrase_candidates(tu, (8,), "run")),
        )
        self.assertIn(
            cross_context,
            set(scoped_phrase_candidates(tu, (8,), "run-and-tu")),
        )
        package = package_from_keys([cross_context], len(cross_context))
        self.assertEqual(
            scoped_materialization_keys(tu, package, "run-and-tu"),
            [cross_context],
        )
        coverage_keys = coverage_materialization_keys(
            tu,
            package,
            "run-and-tu",
        )
        self.assertEqual(
            [region for key in coverage_keys for region in decode_key(key)],
            expected_regions(tu),
        )

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
            "run",
            "greedy",
            0,
            0,
            False,
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

        dual_match = evaluate_online(
            tus,
            "run-vs-tu-match-test",
            empty,
            empty_frame,
            3,
            (8,),
            "run-vs-tu-match",
            "greedy",
            0,
            0,
            False,
            2,
            4_096,
            4_096,
            100,
            8_192,
            "first-use",
            "ids32",
        )
        self.assertTrue(dual_match.exact)
        self.assertLessEqual(
            dual_match.as_dict()["charged_wire_bytes"],
            result.as_dict()["charged_wire_bytes"],
        )

        context_match = evaluate_online(
            tus,
            "run-vs-tu-match-context-test",
            empty,
            empty_frame,
            3,
            (8,),
            "run-vs-tu-match-context",
            "greedy",
            0,
            0,
            False,
            2,
            4_096,
            4_096,
            100,
            8_192,
            "first-use",
            "ids32",
        )
        self.assertTrue(context_match.exact)
        self.assertGreater(context_match.context_wire_bytes, 0)

        hybrid_match = evaluate_online(
            tus,
            "run-vs-tu-match-hybrid-test",
            empty,
            empty_frame,
            3,
            (8,),
            "run-vs-tu-match-hybrid",
            "greedy",
            0,
            0,
            False,
            2,
            4_096,
            4_096,
            100,
            8_192,
            "first-use",
            "ids32",
        )
        self.assertTrue(hybrid_match.exact)
        self.assertLessEqual(
            hybrid_match.as_dict()["charged_wire_bytes"],
            context_match.as_dict()["charged_wire_bytes"],
        )


if __name__ == "__main__":
    unittest.main()
