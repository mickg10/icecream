#!/usr/bin/env python3
"""Exact causal variable-length copies from prior byte roots."""

from __future__ import annotations

import collections
import dataclasses
from typing import Sequence

from line_stream_ceiling import get_varint, put_varint


FORMAT = 1
LITERAL = 0
COPY_ROOT = 1
MASK64 = (1 << 64) - 1
HASH_BASE = 0x9E3779B185EBCA87


def seed_hash(value: bytes, position: int, length: int) -> int:
    result = 0
    for byte in value[position:position + length]:
        result = (result * HASH_BASE + byte + 1) & MASK64
    return result


def roll_hash(current: int, old: int, new: int, outgoing_power: int) -> int:
    current = (current - (old + 1) * outgoing_power) & MASK64
    return (current * HASH_BASE + new + 1) & MASK64


@dataclasses.dataclass(frozen=True, slots=True)
class CopyEdge:
    root: int
    start: int
    length: int


class PriorByteIndex:
    def __init__(self, seed_length: int = 16, candidates: int = 4, stride: int = 8):
        if seed_length < 4 or candidates <= 0 or stride <= 0:
            raise ValueError("prior-byte index controls must be positive")
        self.seed_length = seed_length
        self.candidates = candidates
        self.stride = stride
        self.outgoing_power = pow(HASH_BASE, seed_length - 1, 1 << 64)
        self.roots: list[bytes] = []
        self.index: dict[int, collections.deque[tuple[int, int]]] = {}
        self.root_bytes = 0
        self.index_entries = 0
        self.indexed_windows = 0

    def add(self, value: bytes) -> None:
        root_id = len(self.roots)
        self.roots.append(value)
        self.root_bytes += len(value)
        limit = len(value) - self.seed_length + 1
        if limit <= 0:
            return
        current = seed_hash(value, 0, self.seed_length)
        for position in range(limit):
            if position % self.stride == 0:
                bucket = self.index.get(current)
                if bucket is None:
                    bucket = collections.deque(maxlen=self.candidates)
                    self.index[current] = bucket
                if len(bucket) < self.candidates:
                    self.index_entries += 1
                bucket.append((root_id, position))
                self.indexed_windows += 1
            if position + 1 < limit:
                current = roll_hash(
                    current,
                    value[position],
                    value[position + self.seed_length],
                    self.outgoing_power,
                )

    def edges(self, target: bytes, position: int, key: int) -> list[CopyEdge]:
        seed_end = position + self.seed_length
        output: dict[int, CopyEdge] = {}
        for root_id, start in reversed(self.index.get(key, ())):
            root = self.roots[root_id]
            if root[start:start + self.seed_length] != target[position:seed_end]:
                continue
            length = self.seed_length
            maximum = min(len(root) - start, len(target) - position)
            while length < maximum and root[start + length] == target[position + length]:
                length += 1
            edge = CopyEdge(root_id, start, length)
            previous = output.get(length)
            if previous is None or copy_cost(edge, len(self.roots)) < copy_cost(
                previous, len(self.roots)
            ):
                output[length] = edge
        return list(output.values())

    @property
    def receiver_logical_bytes(self) -> int:
        return self.root_bytes + 8 * (len(self.roots) + 1)

    @property
    def index_logical_bytes(self) -> int:
        return 8 * len(self.index) + 8 * self.index_entries


def varint_size(value: int) -> int:
    return len(put_varint(value))


def copy_cost(edge: CopyEdge, root_count: int) -> int:
    return (
        1
        + varint_size(root_count - edge.root)
        + varint_size(edge.start)
        + varint_size(edge.length)
    )


def encode(value: bytes, index: PriorByteIndex) -> tuple[bytes, int, int]:
    choices: list[bytes | CopyEdge] = []
    position = literal_begin = 0
    limit = len(value) - index.seed_length + 1
    current_hash = seed_hash(value, 0, index.seed_length) if limit > 0 else 0
    copies = copied_bytes = 0

    while position < limit:
        best: CopyEdge | None = None
        best_saving = 0
        for edge in index.edges(value, position, current_hash):
            saving = edge.length - copy_cost(edge, len(index.roots))
            if saving > best_saving or (
                saving == best_saving and best is not None and edge.length > best.length
            ):
                best = edge
                best_saving = saving
        if best is not None:
            if literal_begin < position:
                choices.append(value[literal_begin:position])
            choices.append(best)
            position += best.length
            literal_begin = position
            copies += 1
            copied_bytes += best.length
            if position < limit:
                current_hash = seed_hash(value, position, index.seed_length)
            continue
        if position + 1 < limit:
            current_hash = roll_hash(
                current_hash,
                value[position],
                value[position + index.seed_length],
                index.outgoing_power,
            )
        position += 1

    if literal_begin < len(value):
        choices.append(value[literal_begin:])
    output = bytearray(put_varint(FORMAT))
    output += put_varint(len(value))
    for choice in choices:
        if isinstance(choice, bytes):
            output.append(LITERAL)
            output += put_varint(len(choice))
            output += choice
        else:
            output.append(COPY_ROOT)
            output += put_varint(len(index.roots) - choice.root)
            output += put_varint(choice.start)
            output += put_varint(choice.length)
    return bytes(output), copies, copied_bytes


def decode(raw: bytes, roots: Sequence[bytes]) -> bytes:
    version, offset = get_varint(raw, 0)
    if version != FORMAT:
        raise ValueError("unknown prior-byte-copy format")
    size, offset = get_varint(raw, offset)
    output = bytearray()
    while len(output) < size:
        if offset >= len(raw):
            raise ValueError("truncated prior-byte-copy opcode")
        opcode = raw[offset]
        offset += 1
        if opcode == LITERAL:
            length, offset = get_varint(raw, offset)
            end = offset + length
            if end > len(raw) or length > size - len(output):
                raise ValueError("prior-byte literal exceeds input or output")
            output.extend(raw[offset:end])
            offset = end
        elif opcode == COPY_ROOT:
            back, offset = get_varint(raw, offset)
            start, offset = get_varint(raw, offset)
            length, offset = get_varint(raw, offset)
            if not 0 < back <= len(roots):
                raise ValueError("unknown prior byte root")
            root = roots[len(roots) - back]
            if length == 0 or start + length > len(root) or length > size - len(output):
                raise ValueError("prior-byte copy exceeds source or output")
            output.extend(root[start:start + length])
        else:
            raise ValueError("unknown prior-byte-copy opcode")
    if offset != len(raw):
        raise ValueError("trailing prior-byte-copy bytes")
    return bytes(output)
