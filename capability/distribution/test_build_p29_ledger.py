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


if __name__ == "__main__":
    unittest.main()
