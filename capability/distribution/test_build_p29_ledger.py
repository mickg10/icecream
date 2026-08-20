#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("build_p29_ledger.py")
SPEC = importlib.util.spec_from_file_location("build_p29_ledger", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = builder
SPEC.loader.exec_module(builder)


def frame(kind: int, payload: bytes) -> bytes:
    return bytes([kind]) + struct.pack("<I", len(payload)) + payload


class P29LedgerBuilderTest(unittest.TestCase):
    def test_frame_slices_map_to_causal_dialogue_and_tile_each_direction(self) -> None:
        c_data = b"".join(
            (
                frame(1, b"root"),
                frame(2, b"block"),
                frame(7, b"regions"),
                frame(31, b"fallback"),
                frame(0xFE, b"close"),
            )
        )
        f_data = b"".join(
            (
                frame(3, b"need"),
                frame(30, b"request"),
                frame(0xFD, b"ack"),
            )
        )
        c_frames = builder.parse_frames(c_data, 0, len(c_data), "c")
        f_frames = builder.parse_frames(f_data, 0, len(f_data), "f")
        phases = builder.phase_rows(c_frames, f_frames)
        self.assertEqual(
            [row["name"] for row in phases],
            [
                "p29-root",
                "p29-need",
                "p29-fill",
                "p29-fallback-request",
                "p29-fallback-reply",
                "p29-close",
                "p29-ack",
            ],
        )
        self.assertEqual(
            sum(row["bytes"] for row in phases if row["direction"] == "c_to_f"),
            len(c_data),
        )
        self.assertEqual(
            sum(row["bytes"] for row in phases if row["direction"] == "f_to_c"),
            len(f_data),
        )

    def test_frame_parser_rejects_unknown_or_partial_frames(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown type"):
            builder.parse_frames(frame(99, b"x"), 0, 6, "bad")
        with self.assertRaisesRegex(ValueError, "truncated"):
            builder.parse_frames(frame(1, b"abc")[:-1], 0, 7, "short")

    def test_transaction_graph_forks_lines_from_sent_and_need_from_delivery(self) -> None:
        phases = [
            {"name": "p29-root", "direction": "c_to_f", "bytes": 10},
            {"name": "p29-need", "direction": "f_to_c", "bytes": 2},
            {"name": "p29-lines", "direction": "c_to_f", "bytes": 20},
            {"name": "p29-fill", "direction": "c_to_f", "bytes": 3},
            {"name": "p29-close", "direction": "c_to_f", "bytes": 4},
            {"name": "p29-ack", "direction": "f_to_c", "bytes": 5},
        ]
        graph = builder.transaction_graph(phases)
        by_name = {row["name"]: row for row in graph["phases"]}
        self.assertEqual(by_name["p29-lines"]["depends_on"], ["p29-root:sent"])
        self.assertEqual(by_name["p29-need"]["depends_on"], ["p29-root:delivered"])
        self.assertEqual(by_name["p29-fill"]["depends_on"], ["p29-need:delivered"])
        self.assertEqual(
            set(by_name["p29-close"]["depends_on"]),
            {
                "p29-lines:delivered",
                "p29-fill:delivered",
                "p29-need:delivered",
            },
        )
        self.assertEqual(graph["input_ready_after"][-1], "p29-close:delivered")
        self.assertEqual(graph["commit_after"], ["p29-ack:delivered"])

    def test_transaction_graph_waits_for_an_empty_need_response(self) -> None:
        graph = builder.transaction_graph(
            [
                {"name": "p29-root", "direction": "c_to_f", "bytes": 10},
                {"name": "p29-need", "direction": "f_to_c", "bytes": 2},
                {"name": "p29-close", "direction": "c_to_f", "bytes": 4},
            ]
        )
        by_name = {row["name"]: row for row in graph["phases"]}
        self.assertEqual(
            by_name["p29-close"]["depends_on"], ["p29-need:delivered"]
        )


if __name__ == "__main__":
    unittest.main()
