#!/usr/bin/env python3

import random
import tempfile
import unittest
from pathlib import Path

from online_bootstrap_curves import (
    RegionIdStore,
    canonical_atom_frame,
    compress_frame,
    context_run_lengths,
    coverage_materialization_keys,
    decode_canonical_atom_frame,
    deserialize_definition_batch,
    deserialize_context_runs,
    deserialize_mixed_definition_batch,
    evaluate_online,
    scoped_materialization_keys,
    scoped_phrase_candidates,
    serialize_context_runs,
    serialize_key_batch,
    serialize_mixed_definition_batch,
    serialize_reference_batch,
    summarize_learning_gate,
    tu_with_context_runs,
    write_curve,
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
from prior_root_copy import (
    ExactState as PriorRootState,
    PriorRootIndex,
    deserialize as deserialize_prior_root,
    serialize as serialize_prior_root,
)
from prior_byte_copy import PriorByteIndex, decode as decode_prior_bytes, encode as encode_prior_bytes


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
    def test_prior_byte_copy_replays_long_sparse_match(self) -> None:
        first = bytes(range(256)) * 4
        second = b"new-prefix" + first[123:900] + b"new-suffix"
        index = PriorByteIndex(seed_length=16, candidates=4, stride=8)
        first_raw, first_copies, _ = encode_prior_bytes(first, index)
        self.assertEqual(decode_prior_bytes(first_raw, index.roots), first)
        self.assertEqual(first_copies, 0)
        index.add(first)

        second_raw, copies, copied = encode_prior_bytes(second, index)
        self.assertEqual(decode_prior_bytes(second_raw, index.roots), second)
        self.assertGreater(copies, 0)
        self.assertGreaterEqual(copied, 760)
        with self.assertRaises(ValueError):
            decode_prior_bytes(second_raw[:-1], index.roots)

    def test_prior_root_codec_replays_exact_variable_length_copy(self) -> None:
        first = [(100 + i, 200 + i, 16) for i in range(24)]
        second = [
            (999, 1_999, 32),
            *first[3:21],
            (1_000, 2_000, 32),
        ]
        sender = PriorRootState()
        receiver = PriorRootState()
        index = PriorRootIndex(4, 4)

        first_raw, first_new, _, _ = serialize_prior_root(
            first, sender, index, "greedy"
        )
        first_recovered, first_received = deserialize_prior_root(first_raw, receiver)
        self.assertEqual(first_recovered, first)
        sender.commit(first, first_new)
        receiver.commit(first_recovered, first_received)
        index.add(first)

        second_raw, second_new, copies, copied = serialize_prior_root(
            second, sender, index, "greedy"
        )
        second_recovered, second_received = deserialize_prior_root(second_raw, receiver)
        self.assertEqual(second_recovered, second)
        self.assertEqual(second_received, second_new)
        self.assertEqual(copies, 1)
        self.assertEqual(copied, 18)
        with self.assertRaises(ValueError):
            deserialize_prior_root(second_raw[:-1], receiver)
        self.assertEqual(index.root_regions, len(first))
        self.assertGreater(index.indexed_windows, 0)
        self.assertGreater(index.encoder_logical_bytes, index.root_vector_logical_bytes)

    def test_prior_root_sparse_index_retains_long_match(self) -> None:
        first = [(100 + i, 200 + i, 16) for i in range(24)]
        second = [(999, 1_999, 32), *first, (1_000, 2_000, 32)]
        sender = PriorRootState()
        receiver = PriorRootState()
        index = PriorRootIndex(4, 4, stride=8)
        first_raw, first_new, _, _ = serialize_prior_root(
            first, sender, index, "greedy"
        )
        first_recovered, first_received = deserialize_prior_root(first_raw, receiver)
        sender.commit(first, first_new)
        receiver.commit(first_recovered, first_received)
        index.add(first)

        second_raw, second_new, copies, copied = serialize_prior_root(
            second, sender, index, "greedy"
        )
        recovered, received = deserialize_prior_root(second_raw, receiver)
        self.assertEqual(recovered, second)
        self.assertEqual(received, second_new)
        self.assertGreater(copies, 0)
        self.assertGreaterEqual(copied, len(first) - index.stride + 1)

    def test_prior_root_codec_survives_causal_mutation_and_reorder(self) -> None:
        randomizer = random.Random(0x16C0DE)
        for parse_policy in ("greedy", "dp"):
            sender = PriorRootState()
            receiver = PriorRootState()
            index = PriorRootIndex(4, 4, stride=3)
            root = [(100 + i, 1_000 + i, 8 + i % 7) for i in range(64)]
            for ordinal in range(40):
                if ordinal:
                    left = randomizer.randrange(0, len(root) - 8)
                    right = randomizer.randrange(left + 4, len(root))
                    middle = root[left:right]
                    if ordinal % 3 == 0:
                        middle = list(reversed(middle))
                    new = (
                        10_000 + ordinal,
                        20_000 + ordinal,
                        16 + ordinal % 11,
                    )
                    root = [*root[:left], new, *middle, *root[right:]]

                raw, definitions, _, _ = serialize_prior_root(
                    root,
                    sender,
                    index,
                    parse_policy,
                )
                recovered, received = deserialize_prior_root(raw, receiver)
                self.assertEqual(recovered, root)
                self.assertEqual(received, definitions)
                sender.commit(root, definitions)
                receiver.commit(recovered, received)
                index.add(root)
                self.assertEqual(sender.ids, receiver.ids)
                self.assertEqual(sender.values, receiver.values)
                self.assertEqual(sender.roots, receiver.roots)

    def test_mixed_seed_definition_batch_uses_dense_and_exact_forms(self) -> None:
        known = [(10 + i, 20 + i, 16) for i in range(4)]
        unseen = [(100 + i, 200 + i, 32) for i in range(4)]
        known_key = encode_regions(known)
        unseen_key = encode_regions(unseen)
        sender = RegionIdStore()
        sender.observe(known)
        encoded = serialize_mixed_definition_batch(
            [known_key, unseen_key],
            sender,
        )

        receiver = RegionIdStore()
        receiver.observe(known)
        self.assertEqual(
            deserialize_definition_batch(encoded, receiver),
            [known_key, unseen_key],
        )
        self.assertEqual(
            deserialize_definition_batch(
                serialize_reference_batch([known_key], sender),
                receiver,
            ),
            [known_key],
        )
        with self.assertRaises(ValueError):
            deserialize_mixed_definition_batch(encoded[:-1], receiver)

    def test_c_only_seed_publishes_unseen_regions_with_winning_tu(self) -> None:
        phrase_regions = [(100 + i, 200 + i, 16) for i in range(8)]
        phrase = encode_regions(phrase_regions)
        seed = package_from_keys([phrase], len(phrase))
        seed_frame = compress_frame(serialize_key_batch(seed.keys), 3)
        tus = [
            TuRegions(
                tu,
                1_000_000,
                [
                    (h1, h2, 1, raw_len)
                    for h1, h2, raw_len in phrase_regions * 32
                ]
                + [(1_000 + tu, 2_000 + tu, 1, 16)],
            )
            for tu in range(1, 4)
        ]

        result = evaluate_online(
            tus,
            "seed-only-test",
            seed,
            seed_frame,
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
            True,
        )

        self.assertTrue(result.exact)
        self.assertEqual(result.pretrained_mode, "seed-only")
        self.assertEqual(result.initial_assets, 0)
        self.assertEqual(result.initial_model_wire_bytes, 0)
        self.assertEqual(result.seed_assets, 1)
        self.assertGreater(result.seed_model_compressed_bytes, 0)
        self.assertEqual(result.seed_published_assets, 1)
        self.assertGreater(result.online_selected_tus, 0)
        self.assertGreater(result.definition_wire_bytes, 0)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "curve.tsv"
            write_curve([result.as_dict()], str(path))
            lines = path.read_text().splitlines()
        self.assertIn("seed_published_assets", lines[0].split("\t"))
        self.assertEqual(len(lines), len(tus) + 1)

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

        root_copy_match = evaluate_online(
            tus,
            "run-vs-tu-match-hybrid-root-copy-test",
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
            prior_root_copy=True,
        )
        self.assertTrue(root_copy_match.exact)
        self.assertGreater(root_copy_match.root_copy_selected_tus, 0)
        self.assertGreater(root_copy_match.root_copy_copied_regions, 0)
        self.assertLessEqual(
            root_copy_match.as_dict()["charged_wire_bytes"],
            hybrid_match.as_dict()["charged_wire_bytes"],
        )


if __name__ == "__main__":
    unittest.main()
