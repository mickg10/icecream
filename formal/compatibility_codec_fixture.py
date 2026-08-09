#!/usr/bin/env python3
"""Deterministic byte-level companion for mixed-version formal checks.

This is a small generated-fixture codec, not the production Icecream codec.
It gives the formal compatibility layer executable byte-boundary obligations:

* an OLD peer receives exactly the frozen OLD payload shape;
* a NEW client assigned to an OLD worker projects to byte-identical OLD data;
* TOKEN fields are impossible in an OLD frame;
* partial prefixes/bodies and trailing bytes are rejected by exact decoding;
* stream decoding consumes one complete length-delimited frame at a time; and
* terminal extensions are version-gated rather than appended to OLD frames.

The final upstream product gate must map these fields to the real message
classes and run the same assertions over a C++ socketpair fixture.
"""

from __future__ import annotations

import argparse
import dataclasses
import socket
import struct
import sys
from enum import IntEnum
from typing import Final, Sequence

sys.dont_write_bytecode = True

OLD_VERSION: Final[int] = 1
NEW_VERSION: Final[int] = 2
MAX_FRAME: Final[int] = 4096
_LENGTH = struct.Struct("!I")
_OLD_ASSIGN = struct.Struct("!BII")
_NEW_ASSIGN = struct.Struct("!BBIQQ")
_OLD_TERMINAL = struct.Struct("!BII")
_NEW_TERMINAL = struct.Struct("!BBIQQI")


class CodecError(ValueError):
    """A frame is malformed, incomplete, trailing, or version-incompatible."""


class MessageType(IntEnum):
    ASSIGN = 1
    TERMINAL = 2


class Policy(IntEnum):
    LEGACY = 1
    FENCED_LEGACY = 2
    TOKEN = 3


@dataclasses.dataclass(frozen=True)
class Assignment:
    wire_id: int
    worker_port: int
    policy: Policy = Policy.LEGACY
    full_id: int = 0
    token: int = 0


@dataclasses.dataclass(frozen=True)
class Terminal:
    wire_id: int
    status: int
    policy: Policy = Policy.LEGACY
    full_id: int = 0
    token: int = 0
    generation: int = 0


def _bounded(value: int, bits: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise CodecError(f"{name} must be an integer")
    if value < 0 or value >= 1 << bits:
        raise CodecError(f"{name} does not fit unsigned {bits} bits: {value}")
    return value


def _frame(payload: bytes) -> bytes:
    if not payload or len(payload) > MAX_FRAME:
        raise CodecError(f"invalid payload length {len(payload)}")
    return _LENGTH.pack(len(payload)) + payload


def encode_assignment(version: int, assignment: Assignment) -> bytes:
    wire_id = _bounded(assignment.wire_id, 32, "wire_id")
    worker_port = _bounded(assignment.worker_port, 32, "worker_port")
    if version == OLD_VERSION:
        if assignment.policy is not Policy.LEGACY:
            raise CodecError("OLD assignment cannot carry fenced/token policy")
        if assignment.full_id != 0 or assignment.token != 0:
            raise CodecError("OLD assignment cannot carry full_id/token")
        return _frame(
            _OLD_ASSIGN.pack(MessageType.ASSIGN, wire_id, worker_port)
        )
    if version == NEW_VERSION:
        full_id = _bounded(assignment.full_id, 64, "full_id")
        token = _bounded(assignment.token, 64, "token")
        if assignment.policy is Policy.TOKEN:
            if full_id == 0 or token == 0:
                raise CodecError("TOKEN assignment requires nonzero full_id/token")
        elif full_id != 0 or token != 0:
            raise CodecError(
                "non-TOKEN assignment must project full_id/token to zero"
            )
        return _frame(
            _NEW_ASSIGN.pack(
                MessageType.ASSIGN,
                int(assignment.policy),
                wire_id,
                full_id,
                token,
            )
        )
    raise CodecError(f"unsupported version {version}")


def encode_terminal(version: int, terminal: Terminal) -> bytes:
    wire_id = _bounded(terminal.wire_id, 32, "wire_id")
    status = _bounded(terminal.status, 32, "status")
    if version == OLD_VERSION:
        if terminal.policy is not Policy.LEGACY:
            raise CodecError("OLD terminal cannot carry fenced/token policy")
        if terminal.full_id or terminal.token or terminal.generation:
            raise CodecError("OLD terminal cannot carry new identity fields")
        return _frame(
            _OLD_TERMINAL.pack(MessageType.TERMINAL, wire_id, status)
        )
    if version == NEW_VERSION:
        full_id = _bounded(terminal.full_id, 64, "full_id")
        token = _bounded(terminal.token, 64, "token")
        generation = _bounded(terminal.generation, 32, "generation")
        if terminal.policy is Policy.TOKEN:
            if full_id == 0 or token == 0:
                raise CodecError("TOKEN terminal requires nonzero full_id/token")
        elif full_id != 0 or token != 0:
            raise CodecError("non-TOKEN terminal must zero full_id/token")
        return _frame(
            _NEW_TERMINAL.pack(
                MessageType.TERMINAL,
                int(terminal.policy),
                wire_id,
                full_id,
                token,
                generation,
            )
        )
    raise CodecError(f"unsupported version {version}")


def _payload_from_exact_frame(frame: bytes) -> bytes:
    if len(frame) < _LENGTH.size:
        raise CodecError("partial length prefix")
    (payload_length,) = _LENGTH.unpack_from(frame)
    if payload_length == 0 or payload_length > MAX_FRAME:
        raise CodecError(f"invalid announced payload length {payload_length}")
    expected = _LENGTH.size + payload_length
    if len(frame) < expected:
        raise CodecError(
            f"partial frame: expected {expected} bytes, received {len(frame)}"
        )
    if len(frame) > expected:
        raise CodecError(
            f"trailing bytes after exact frame: expected {expected}, "
            f"received {len(frame)}"
        )
    return frame[_LENGTH.size:expected]


def decode_exact(version: int, frame: bytes) -> Assignment | Terminal:
    payload = _payload_from_exact_frame(frame)
    if not payload:
        raise CodecError("empty payload")
    message_type = payload[0]
    if message_type == MessageType.ASSIGN:
        return _decode_assignment_payload(version, payload)
    if message_type == MessageType.TERMINAL:
        return _decode_terminal_payload(version, payload)
    raise CodecError(f"unknown message type {message_type}")


def _decode_assignment_payload(version: int, payload: bytes) -> Assignment:
    if version == OLD_VERSION:
        if len(payload) != _OLD_ASSIGN.size:
            raise CodecError(
                f"OLD assignment payload length {len(payload)} != "
                f"{_OLD_ASSIGN.size}"
            )
        message_type, wire_id, worker_port = _OLD_ASSIGN.unpack(payload)
        if message_type != MessageType.ASSIGN:
            raise CodecError("wrong assignment message type")
        return Assignment(wire_id=wire_id, worker_port=worker_port)
    if version == NEW_VERSION:
        if len(payload) != _NEW_ASSIGN.size:
            raise CodecError(
                f"NEW assignment payload length {len(payload)} != "
                f"{_NEW_ASSIGN.size}"
            )
        message_type, raw_policy, wire_id, full_id, token = _NEW_ASSIGN.unpack(
            payload
        )
        if message_type != MessageType.ASSIGN:
            raise CodecError("wrong assignment message type")
        try:
            policy = Policy(raw_policy)
        except ValueError as exc:
            raise CodecError(f"unknown assignment policy {raw_policy}") from exc
        assignment = Assignment(
            wire_id=wire_id,
            worker_port=0,
            policy=policy,
            full_id=full_id,
            token=token,
        )
        if policy is Policy.TOKEN and (full_id == 0 or token == 0):
            raise CodecError("decoded TOKEN assignment has zero identity")
        if policy is not Policy.TOKEN and (full_id != 0 or token != 0):
            raise CodecError("decoded non-TOKEN assignment has identity fields")
        return assignment
    raise CodecError(f"unsupported version {version}")


def _decode_terminal_payload(version: int, payload: bytes) -> Terminal:
    if version == OLD_VERSION:
        if len(payload) != _OLD_TERMINAL.size:
            raise CodecError(
                f"OLD terminal payload length {len(payload)} != "
                f"{_OLD_TERMINAL.size}"
            )
        message_type, wire_id, status = _OLD_TERMINAL.unpack(payload)
        if message_type != MessageType.TERMINAL:
            raise CodecError("wrong terminal message type")
        return Terminal(wire_id=wire_id, status=status)
    if version == NEW_VERSION:
        if len(payload) != _NEW_TERMINAL.size:
            raise CodecError(
                f"NEW terminal payload length {len(payload)} != "
                f"{_NEW_TERMINAL.size}"
            )
        (
            message_type,
            raw_policy,
            wire_id,
            full_id,
            token,
            generation,
        ) = _NEW_TERMINAL.unpack(payload)
        if message_type != MessageType.TERMINAL:
            raise CodecError("wrong terminal message type")
        try:
            policy = Policy(raw_policy)
        except ValueError as exc:
            raise CodecError(f"unknown terminal policy {raw_policy}") from exc
        terminal = Terminal(
            wire_id=wire_id,
            status=0,
            policy=policy,
            full_id=full_id,
            token=token,
            generation=generation,
        )
        if policy is Policy.TOKEN and (full_id == 0 or token == 0):
            raise CodecError("decoded TOKEN terminal has zero identity")
        if policy is not Policy.TOKEN and (full_id != 0 or token != 0):
            raise CodecError("decoded non-TOKEN terminal has identity fields")
        return terminal
    raise CodecError(f"unsupported version {version}")


def pop_stream_frame(buffer: bytes) -> tuple[bytes, bytes]:
    """Return exactly one frame and the untouched suffix."""
    if len(buffer) < _LENGTH.size:
        raise CodecError("partial stream length prefix")
    (payload_length,) = _LENGTH.unpack_from(buffer)
    if payload_length == 0 or payload_length > MAX_FRAME:
        raise CodecError(f"invalid stream payload length {payload_length}")
    frame_length = _LENGTH.size + payload_length
    if len(buffer) < frame_length:
        raise CodecError("partial stream frame")
    return buffer[:frame_length], buffer[frame_length:]


def legacy_projection(assignment: Assignment) -> Assignment:
    """Project a compatible new-client request onto the frozen OLD envelope."""
    if assignment.policy is not Policy.LEGACY:
        raise CodecError("only Legacy policy can project to OLD")
    return Assignment(
        wire_id=assignment.wire_id,
        worker_port=assignment.worker_port,
        policy=Policy.LEGACY,
        full_id=0,
        token=0,
    )


def socketpair_roundtrip(
    version: int, frame: bytes, *, chunk_sizes: Sequence[int] = ()
) -> Assignment | Terminal:
    left, right = socket.socketpair()
    try:
        if chunk_sizes:
            cursor = 0
            for size in chunk_sizes:
                if size <= 0:
                    raise CodecError("chunk size must be positive")
                part = frame[cursor:cursor + size]
                if not part:
                    break
                left.sendall(part)
                cursor += len(part)
            if cursor < len(frame):
                left.sendall(frame[cursor:])
        else:
            left.sendall(frame)
        left.shutdown(socket.SHUT_WR)
        received = bytearray()
        while True:
            chunk = right.recv(7)
            if not chunk:
                break
            received.extend(chunk)
        return decode_exact(version, bytes(received))
    finally:
        left.close()
        right.close()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emit-old-assignment-hex",
        action="store_true",
        help="print the frozen OLD assignment fixture as lowercase hex",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.emit_old_assignment_hex:
        frame = encode_assignment(
            OLD_VERSION,
            Assignment(wire_id=0x01020304, worker_port=8765),
        )
        print(frame.hex())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
