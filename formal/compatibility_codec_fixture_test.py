#!/usr/bin/env python3
"""Self-tests for compatibility_codec_fixture.py; no external packages."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

MODULE_PATH = Path(__file__).with_name("compatibility_codec_fixture.py")
SPEC = importlib.util.spec_from_file_location("compatibility_codec_fixture", MODULE_PATH)
assert SPEC and SPEC.loader
codec = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = codec
SPEC.loader.exec_module(codec)


class CompatibilityCodecTests(unittest.TestCase):
    def test_frozen_old_assignment_bytes_and_socketpair_roundtrip(self) -> None:
        assignment = codec.Assignment(wire_id=0x01020304, worker_port=8765)
        frame = codec.encode_assignment(codec.OLD_VERSION, assignment)
        self.assertEqual(frame.hex(), "0000000901010203040000223d")
        self.assertEqual(
            codec.socketpair_roundtrip(
                codec.OLD_VERSION,
                frame,
                chunk_sizes=(1, 2, 1, 3),
            ),
            assignment,
        )

    def test_new_token_assignment_roundtrip_preserves_common_and_new_fields(self) -> None:
        assignment = codec.Assignment(
            wire_id=77,
            worker_port=8766,
            policy=codec.Policy.TOKEN,
            full_id=0x102030405060708,
            token=0x8877665544332211,
        )
        frame = codec.encode_assignment(codec.NEW_VERSION, assignment)
        self.assertEqual(
            codec.socketpair_roundtrip(
                codec.NEW_VERSION,
                frame,
                chunk_sizes=(4, 1, 5, 2, 7),
            ),
            assignment,
        )

    def test_new_client_legacy_projection_is_byte_identical_to_old_encoder(self) -> None:
        new_client_request = codec.Assignment(
            wire_id=991,
            worker_port=8765,
            policy=codec.Policy.LEGACY,
        )
        projected = codec.legacy_projection(new_client_request)
        self.assertEqual(
            codec.encode_assignment(codec.OLD_VERSION, projected),
            codec.encode_assignment(codec.OLD_VERSION, new_client_request),
        )
        self.assertEqual(
            codec.decode_exact(
                codec.OLD_VERSION,
                codec.encode_assignment(codec.OLD_VERSION, projected),
            ),
            projected,
        )

    def test_old_encoder_rejects_new_policy_or_identity_fields(self) -> None:
        with self.assertRaisesRegex(codec.CodecError, "OLD assignment"):
            codec.encode_assignment(
                codec.OLD_VERSION,
                codec.Assignment(
                    wire_id=1,
                    worker_port=2,
                    policy=codec.Policy.TOKEN,
                    full_id=3,
                    token=4,
                ),
            )
        with self.assertRaisesRegex(codec.CodecError, "full_id/token"):
            codec.encode_assignment(
                codec.OLD_VERSION,
                codec.Assignment(
                    wire_id=1,
                    worker_port=2,
                    policy=codec.Policy.LEGACY,
                    full_id=3,
                ),
            )

    def test_old_decoder_rejects_new_assignment_shape(self) -> None:
        frame = codec.encode_assignment(
            codec.NEW_VERSION,
            codec.Assignment(
                wire_id=1,
                worker_port=2,
                policy=codec.Policy.TOKEN,
                full_id=3,
                token=4,
            ),
        )
        with self.assertRaisesRegex(codec.CodecError, "OLD assignment payload length"):
            codec.decode_exact(codec.OLD_VERSION, frame)

    def test_exact_decoder_rejects_partial_prefix_body_and_trailing_bytes(self) -> None:
        frame = codec.encode_assignment(
            codec.OLD_VERSION,
            codec.Assignment(wire_id=9, worker_port=10),
        )
        for prefix in range(0, 4):
            with self.subTest(prefix=prefix):
                with self.assertRaisesRegex(codec.CodecError, "partial length prefix"):
                    codec.decode_exact(codec.OLD_VERSION, frame[:prefix])
        for length in range(4, len(frame)):
            with self.subTest(length=length):
                with self.assertRaisesRegex(codec.CodecError, "partial frame"):
                    codec.decode_exact(codec.OLD_VERSION, frame[:length])
        with self.assertRaisesRegex(codec.CodecError, "trailing bytes"):
            codec.decode_exact(codec.OLD_VERSION, frame + b"x")

    def test_stream_decoder_consumes_one_frame_and_preserves_successor(self) -> None:
        first = codec.encode_assignment(
            codec.OLD_VERSION,
            codec.Assignment(wire_id=1, worker_port=2),
        )
        second = codec.encode_terminal(
            codec.OLD_VERSION,
            codec.Terminal(wire_id=1, status=0),
        )
        frame, remainder = codec.pop_stream_frame(first + second)
        self.assertEqual(frame, first)
        self.assertEqual(remainder, second)
        self.assertEqual(
            codec.decode_exact(codec.OLD_VERSION, frame),
            codec.Assignment(wire_id=1, worker_port=2),
        )
        terminal_frame, tail = codec.pop_stream_frame(remainder)
        self.assertEqual(tail, b"")
        self.assertEqual(
            codec.decode_exact(codec.OLD_VERSION, terminal_frame),
            codec.Terminal(wire_id=1, status=0),
        )

    def test_old_terminal_rejects_new_identity_extension(self) -> None:
        old = codec.Terminal(wire_id=101, status=7)
        old_frame = codec.encode_terminal(codec.OLD_VERSION, old)
        self.assertEqual(codec.decode_exact(codec.OLD_VERSION, old_frame), old)

        new = codec.Terminal(
            wire_id=101,
            status=7,
            policy=codec.Policy.TOKEN,
            full_id=1001,
            token=2002,
            generation=3,
        )
        new_frame = codec.encode_terminal(codec.NEW_VERSION, new)
        self.assertEqual(codec.decode_exact(codec.NEW_VERSION, new_frame), new)
        with self.assertRaisesRegex(codec.CodecError, "OLD terminal payload length"):
            codec.decode_exact(codec.OLD_VERSION, new_frame)

    def test_new_non_token_frame_must_zero_identity_fields(self) -> None:
        with self.assertRaisesRegex(codec.CodecError, "non-TOKEN assignment"):
            codec.encode_assignment(
                codec.NEW_VERSION,
                codec.Assignment(
                    wire_id=1,
                    worker_port=2,
                    policy=codec.Policy.FENCED_LEGACY,
                    full_id=99,
                ),
            )
        with self.assertRaisesRegex(codec.CodecError, "non-TOKEN terminal"):
            codec.encode_terminal(
                codec.NEW_VERSION,
                codec.Terminal(
                    wire_id=1,
                    status=2,
                    policy=codec.Policy.FENCED_LEGACY,
                    token=88,
                ),
            )


if __name__ == "__main__":
    unittest.main()
