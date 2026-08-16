#!/usr/bin/env python3
"""Exact causal prior-root substring codec and standalone capability probe."""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import random
import time
from pathlib import Path
from typing import Sequence

import zstandard as zstd

from pretrained_superblocks import (
    TuRegions,
    decode_key,
    encode_regions,
    expected_regions,
    get_varint,
    iter_tus_fast,
    put_varint,
)


DEFINE = 0
REFERENCE = 1
COPY_ROOT = 2
FORMAT = 1
MASK64 = (1 << 64) - 1


def rotate_left(value: int, bits: int) -> int:
    return ((value << bits) | (value >> (64 - bits))) & MASK64


def mix64(value: int) -> int:
    value ^= value >> 30
    value = value * 0xBF58476D1CE4E5B9 & MASK64
    value ^= value >> 27
    value = value * 0x94D049BB133111EB & MASK64
    return value ^ (value >> 31)


def region_hash(region: tuple[int, int, int]) -> int:
    return mix64(region[0] ^ rotate_left(region[1], 17) ^ region[2])


def window_hash(
    regions: Sequence[tuple[int, int, int]], position: int, length: int
) -> int:
    value = 0x9E3779B97F4A7C15 ^ length
    for offset in range(length):
        value = mix64(value ^ region_hash(regions[position + offset]) ^ offset)
    return value


@dataclasses.dataclass(slots=True)
class ExactState:
    ids: dict[tuple[int, int, int], int] = dataclasses.field(default_factory=dict)
    values: list[tuple[int, int, int] | None] = dataclasses.field(
        default_factory=lambda: [None]
    )
    roots: list[list[tuple[int, int, int]]] = dataclasses.field(default_factory=list)

    def commit(
        self,
        root: Sequence[tuple[int, int, int]],
        new_regions: Sequence[tuple[int, int, int]],
    ) -> None:
        expected = []
        seen = set(self.ids)
        for region in root:
            if region not in seen:
                expected.append(region)
                seen.add(region)
        if list(new_regions) != expected:
            raise ValueError("frame definitions are not in canonical first-observation order")
        for region in new_regions:
            self.ids[region] = len(self.values)
            self.values.append(region)
        self.roots.append(list(root))


@dataclasses.dataclass(frozen=True, slots=True)
class CopyEdge:
    root: int
    start: int
    length: int


class PriorRootIndex:
    def __init__(self, seed_length: int, candidates: int, stride: int = 1):
        if seed_length < 2 or candidates <= 0 or stride <= 0:
            raise ValueError("prior-root index controls must be positive")
        self.seed_length = seed_length
        self.candidates = candidates
        self.stride = stride
        self.roots: list[list[tuple[int, int, int]]] = []
        self.index: dict[int, collections.deque[tuple[int, int]]] = {}
        self.root_regions = 0
        self.index_entries = 0
        self.indexed_windows = 0

    def add(self, root: Sequence[tuple[int, int, int]]) -> None:
        root_id = len(self.roots)
        stored = list(root)
        self.roots.append(stored)
        self.root_regions += len(stored)
        for position in range(0, len(stored) - self.seed_length + 1, self.stride):
            key = window_hash(stored, position, self.seed_length)
            bucket = self.index.get(key)
            if bucket is None:
                bucket = collections.deque(maxlen=self.candidates)
                self.index[key] = bucket
            if len(bucket) < self.candidates:
                self.index_entries += 1
            bucket.append((root_id, position))
            self.indexed_windows += 1

    @property
    def root_vector_logical_bytes(self) -> int:
        """Packed u32 Region IDs plus u64 root offsets at either endpoint."""
        return 4 * self.root_regions + 8 * (len(self.roots) + 1)

    @property
    def index_logical_bytes(self) -> int:
        """Packed u64 fingerprints and pairs of u32 root/position locations at C."""
        return 8 * len(self.index) + 8 * self.index_entries

    @property
    def encoder_logical_bytes(self) -> int:
        return self.root_vector_logical_bytes + self.index_logical_bytes

    def edges(
        self, target: Sequence[tuple[int, int, int]], position: int
    ) -> list[CopyEdge]:
        if position + self.seed_length > len(target):
            return []
        key = window_hash(target, position, self.seed_length)
        output: list[CopyEdge] = []
        seen_lengths: set[int] = set()
        for root_id, start in reversed(self.index.get(key, ())):
            root = self.roots[root_id]
            if root[start:start + self.seed_length] != target[
                position:position + self.seed_length
            ]:
                continue
            length = self.seed_length
            maximum = min(len(root) - start, len(target) - position)
            while length < maximum and root[start + length] == target[position + length]:
                length += 1
            if length in seen_lengths:
                continue
            seen_lengths.add(length)
            output.append(CopyEdge(root_id, start, length))
        return output


def varint_size(value: int) -> int:
    return len(put_varint(value))


def atom_ids(
    target: Sequence[tuple[int, int, int]], state: ExactState
) -> tuple[list[int], list[bytes], list[tuple[int, int, int]]]:
    transient: dict[tuple[int, int, int], int] = {}
    keys: list[bytes] = []
    new_regions: list[tuple[int, int, int]] = []
    ids: list[int] = []
    for region in target:
        region_id = state.ids.get(region)
        if region_id is None:
            region_id = transient.get(region)
        if region_id is None:
            region_id = len(state.values) + len(transient)
            transient[region] = region_id
            new_regions.append(region)
            keys.append(encode_regions((region,)))
        ids.append(region_id)
    return ids, keys, new_regions


def minimum_program(
    target: Sequence[tuple[int, int, int]],
    ids: Sequence[int],
    state: ExactState,
    root_index: PriorRootIndex,
) -> list[int | CopyEdge]:
    count = len(target)
    atom_costs: list[int] = []
    seen_ids: set[int] = set()
    for region, region_id in zip(target, ids):
        if region_id >= len(state.values) and region_id not in seen_ids:
            key = encode_regions((region,))
            atom_costs.append(
                1 + varint_size(region_id) + varint_size(len(key)) + len(key)
            )
        else:
            atom_costs.append(1 + varint_size(region_id))
        seen_ids.add(region_id)
    costs = [0] * (count + 1)
    choices: list[int | CopyEdge] = [0] * count
    for position in range(count - 1, -1, -1):
        costs[position] = atom_costs[position] + costs[position + 1]
        for edge in root_index.edges(target, position):
            root_back = len(root_index.roots) - edge.root
            wire = (
                1
                + varint_size(root_back)
                + varint_size(edge.start)
                + varint_size(edge.length)
                + costs[position + edge.length]
            )
            if wire < costs[position]:
                costs[position] = wire
                choices[position] = edge
    program: list[int | CopyEdge] = []
    position = 0
    while position < count:
        choice = choices[position]
        program.append(choice)
        position += choice.length if isinstance(choice, CopyEdge) else 1
    return program


def greedy_program(
    target: Sequence[tuple[int, int, int]],
    ids: Sequence[int],
    state: ExactState,
    root_index: PriorRootIndex,
) -> list[int | CopyEdge]:
    atom_costs: list[int] = []
    seen_ids: set[int] = set()
    for region, region_id in zip(target, ids):
        if region_id >= len(state.values) and region_id not in seen_ids:
            key = encode_regions((region,))
            atom_costs.append(
                1 + varint_size(region_id) + varint_size(len(key)) + len(key)
            )
        else:
            atom_costs.append(1 + varint_size(region_id))
        seen_ids.add(region_id)
    prefix_cost = [0]
    for cost in atom_costs:
        prefix_cost.append(prefix_cost[-1] + cost)

    program: list[int | CopyEdge] = []
    position = 0
    while position < len(target):
        best: CopyEdge | None = None
        best_saving = 0
        for edge in root_index.edges(target, position):
            copy_cost = (
                1
                + varint_size(len(root_index.roots) - edge.root)
                + varint_size(edge.start)
                + varint_size(edge.length)
            )
            literal_cost = prefix_cost[position + edge.length] - prefix_cost[position]
            saving = literal_cost - copy_cost
            if saving > best_saving or (
                saving == best_saving and best is not None and edge.length > best.length
            ):
                best = edge
                best_saving = saving
        if best is None:
            program.append(0)
            position += 1
        else:
            program.append(best)
            position += best.length
    return program


def serialize(
    target: Sequence[tuple[int, int, int]],
    state: ExactState,
    root_index: PriorRootIndex | None,
    parse_policy: str,
) -> tuple[bytes, list[tuple[int, int, int]], int, int]:
    ids, keys, new_regions = atom_ids(target, state)
    key_by_id = {
        len(state.values) + offset: key for offset, key in enumerate(keys)
    }
    if root_index is None:
        program: list[int | CopyEdge] = [0] * len(target)
    elif parse_policy == "dp":
        program = minimum_program(target, ids, state, root_index)
    elif parse_policy == "greedy":
        program = greedy_program(target, ids, state, root_index)
    else:
        raise ValueError(f"unknown parse policy: {parse_policy}")
    output = bytearray(put_varint(FORMAT))
    output += put_varint(len(target))
    position = 0
    copied = copies = 0
    emitted_definitions: set[int] = set()
    for choice in program:
        if isinstance(choice, CopyEdge):
            output.append(COPY_ROOT)
            output += put_varint(len(state.roots) - choice.root)
            output += put_varint(choice.start)
            output += put_varint(choice.length)
            position += choice.length
            copied += choice.length
            copies += 1
            continue
        region_id = ids[position]
        key = key_by_id.get(region_id)
        if key is not None and region_id not in emitted_definitions:
            output.append(DEFINE)
            output += put_varint(region_id)
            output += put_varint(len(key))
            output += key
            emitted_definitions.add(region_id)
        else:
            output.append(REFERENCE)
            output += put_varint(region_id)
        position += 1
    return bytes(output), new_regions, copies, copied


def deserialize(
    raw: bytes, state: ExactState
) -> tuple[list[tuple[int, int, int]], list[tuple[int, int, int]]]:
    version, offset = get_varint(raw, 0)
    if version != FORMAT:
        raise ValueError("unknown prior-root-copy format")
    count, offset = get_varint(raw, offset)
    output: list[tuple[int, int, int]] = []
    transient: list[tuple[int, int, int]] = []
    while len(output) < count:
        if offset >= len(raw):
            raise ValueError("truncated prior-root-copy opcode")
        opcode = raw[offset]
        offset += 1
        if opcode == DEFINE:
            region_id, offset = get_varint(raw, offset)
            size, offset = get_varint(raw, offset)
            if offset + size > len(raw):
                raise ValueError("truncated Region definition")
            decoded = decode_key(raw[offset:offset + size])
            offset += size
            expected_id = len(state.values) + len(transient)
            if region_id != expected_id or len(decoded) != 1:
                raise ValueError("noncanonical Region definition")
            region = decoded[0]
            if region in state.ids or region in transient:
                raise ValueError("duplicate Region definition")
            transient.append(region)
            output.append(region)
        elif opcode == REFERENCE:
            region_id, offset = get_varint(raw, offset)
            if 0 < region_id < len(state.values):
                region = state.values[region_id]
            else:
                transient_id = region_id - len(state.values)
                if not 0 <= transient_id < len(transient):
                    raise ValueError("unknown Region reference")
                region = transient[transient_id]
            if region is None:
                raise ValueError("null Region reference")
            output.append(region)
        elif opcode == COPY_ROOT:
            back, offset = get_varint(raw, offset)
            start, offset = get_varint(raw, offset)
            length, offset = get_varint(raw, offset)
            if not 0 < back <= len(state.roots):
                raise ValueError("unknown prior root")
            root = state.roots[len(state.roots) - back]
            if length == 0 or start + length > len(root):
                raise ValueError("prior-root copy exceeds source")
            output.extend(root[start:start + length])
        else:
            raise ValueError("unknown prior-root-copy opcode")
        if len(output) > count:
            raise ValueError("prior-root-copy output exceeds declared count")
    if offset != len(raw):
        raise ValueError("trailing prior-root-copy bytes")
    return output, transient


def load_target(path: str, max_tus: int, order: str, seed: int) -> list[TuRegions]:
    target = list(iter_tus_fast(path))
    if max_tus:
        target = target[:max_tus]
    if order == "reverse":
        target.reverse()
    elif order == "shuffle":
        random.Random(seed).shuffle(target)
    return target


def run(args: argparse.Namespace) -> int:
    target = load_target(args.trace, args.max_tus, args.order, args.order_seed)
    if args.endpoints <= 0:
        raise ValueError("endpoints must be positive")
    encoders = [ExactState() for _ in range(args.endpoints)]
    baseline_decoders = [ExactState() for _ in range(args.endpoints)]
    copy_decoders = [ExactState() for _ in range(args.endpoints)]
    root_indexes = [
        PriorRootIndex(args.seed_length, args.candidates, args.index_stride)
        for _ in range(args.endpoints)
    ]
    assignment_random = random.Random(args.assignment_seed)
    compressors = {level: zstd.ZstdCompressor(level=level) for level in args.levels}
    totals = {
        level: {
            "baseline_wire_bytes": 0,
            "copy_wire_bytes": 0,
            "selected_wire_bytes": 0,
            "copy_selected_tus": 0,
        }
        for level in args.levels
    }
    raw_bytes = regions = copies = copied_regions = baseline_raw = copy_raw = 0
    started = time.monotonic()
    for ordinal, tu in enumerate(target, 1):
        endpoint = (
            (ordinal - 1) % args.endpoints
            if args.assignment == "round-robin"
            else assignment_random.randrange(args.endpoints)
        )
        encoder = encoders[endpoint]
        baseline_decoder = baseline_decoders[endpoint]
        copy_decoder = copy_decoders[endpoint]
        root_index = root_indexes[endpoint]
        expected = expected_regions(tu)
        baseline, baseline_new, _, _ = serialize(
            expected, encoder, None, args.parse_policy
        )
        candidate, candidate_new, frame_copies, frame_copied = serialize(
            expected, encoder, root_index, args.parse_policy
        )
        if candidate_new != baseline_new:
            raise ValueError("candidate and baseline Region definitions differ")
        baseline_recovered, baseline_received = deserialize(baseline, baseline_decoder)
        candidate_recovered, candidate_received = deserialize(candidate, copy_decoder)
        if (
            baseline_recovered != expected
            or candidate_recovered != expected
            or baseline_received != baseline_new
            or candidate_received != candidate_new
        ):
            raise ValueError(f"exact replay failed at TU {ordinal}")
        for level, compressor in compressors.items():
            baseline_frame = compressor.compress(baseline)
            copy_frame = compressor.compress(candidate)
            baseline_wire = len(baseline_frame) + 4 + 1
            copy_wire = len(copy_frame) + 4 + 1
            totals[level]["baseline_wire_bytes"] += baseline_wire
            totals[level]["copy_wire_bytes"] += copy_wire
            totals[level]["selected_wire_bytes"] += min(baseline_wire, copy_wire)
            totals[level]["copy_selected_tus"] += copy_wire < baseline_wire
        encoder.commit(expected, baseline_new)
        baseline_decoder.commit(baseline_recovered, baseline_received)
        copy_decoder.commit(candidate_recovered, candidate_received)
        if not (
            encoder.ids == baseline_decoder.ids == copy_decoder.ids
            and encoder.values == baseline_decoder.values == copy_decoder.values
            and encoder.roots == baseline_decoder.roots == copy_decoder.roots
        ):
            raise ValueError("encoder and decoder states diverged")
        root_index.add(expected)
        raw_bytes += tu.raw_bytes
        regions += len(expected)
        copies += frame_copies
        copied_regions += frame_copied
        baseline_raw += len(baseline)
        copy_raw += len(candidate)
        if ordinal % 50 == 0 or ordinal == len(target):
            print(
                f"PASS tu={ordinal}/{len(target)} regions={regions} "
                f"copied={copied_regions} endpoints={args.endpoints}",
                flush=True,
            )
    elapsed = time.monotonic() - started
    distinct_regions = sum(len(encoder.values) - 1 for encoder in encoders)
    root_regions = sum(index.root_regions for index in root_indexes)
    indexed_windows = sum(index.indexed_windows for index in root_indexes)
    index_keys = sum(len(index.index) for index in root_indexes)
    index_entries = sum(index.index_entries for index in root_indexes)
    receiver_state = sum(index.root_vector_logical_bytes for index in root_indexes)
    encoder_index_state = sum(index.index_logical_bytes for index in root_indexes)
    report = {
        "experiment": "exact causal prior-root substring copy",
        "trace": args.trace,
        "order": args.order,
        "order_seed": args.order_seed,
        "tus": len(target),
        "raw_bytes": raw_bytes,
        "regions": regions,
        "distinct_regions_per_endpoint_sum": distinct_regions,
        "seed_length": args.seed_length,
        "candidate_limit": args.candidates,
        "index_stride": args.index_stride,
        "endpoints": args.endpoints,
        "assignment": args.assignment,
        "assignment_seed": args.assignment_seed,
        "parse_policy": args.parse_policy,
        "copies": copies,
        "copied_regions": copied_regions,
        "copied_fraction": copied_regions / max(1, regions),
        "baseline_uncompressed_bytes": baseline_raw,
        "copy_uncompressed_bytes": copy_raw,
        "retained_roots": sum(len(index.roots) for index in root_indexes),
        "retained_root_regions": root_regions,
        "indexed_windows": indexed_windows,
        "retained_index_keys": index_keys,
        "retained_index_entries": index_entries,
        "receiver_root_vector_logical_bytes": receiver_state,
        "encoder_index_logical_bytes": encoder_index_state,
        "encoder_total_logical_bytes": receiver_state + encoder_index_state,
        "maximum_endpoint_receiver_state_bytes": max(
            index.root_vector_logical_bytes for index in root_indexes
        ),
        "maximum_endpoint_encoder_state_bytes": max(
            index.encoder_logical_bytes for index in root_indexes
        ),
        "elapsed_seconds": elapsed,
        "effective_raw_GBps": raw_bytes / max(elapsed, 1e-12) / 1e9,
        "levels": totals,
        "exact": True,
    }
    if args.output:
        Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--trace", required=True)
    value.add_argument("--max-tus", type=int, default=0)
    value.add_argument("--seed-length", type=int, default=4)
    value.add_argument("--candidates", type=int, default=4)
    value.add_argument("--index-stride", type=int, default=1)
    value.add_argument("--endpoints", type=int, default=1)
    value.add_argument(
        "--assignment", choices=("round-robin", "random"), default="round-robin"
    )
    value.add_argument("--assignment-seed", type=int, default=0xFCA11)
    value.add_argument("--parse-policy", choices=("greedy", "dp"), default="greedy")
    value.add_argument("--levels", nargs="+", type=int, choices=(1, 3), default=(1, 3))
    value.add_argument("--order", choices=("standard", "reverse", "shuffle"), default="standard")
    value.add_argument("--order-seed", type=int, default=0x51B10C)
    value.add_argument("--output")
    return value


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
