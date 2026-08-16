#!/usr/bin/env python3
"""Causal exact-phrase bootstrap curves for issue #16.

This is a focused capability harness.  It compares the *same* exact Region-phrase
learner in two initial states:

* empty vocabulary at target TU 0;
* a frozen exact phrase package learned from disjoint projects and charged at TU 0.

Target-local phrases are observed only after their TU has been encoded.  The primary
first-use policy defers publication until a later TU can repay its definitions in the
same frame; the promotion-time policy remains as a comparison.  The receiver
installs definitions from actual compressed frames, decodes every payload with an
independent state object, and verifies the complete Region stream exactly.

The learner is deliberately simple: bounded Space-Saving document-frequency counts
over exact whole-context and 2/4/8/16/32-Region phrases, immutable phrase IDs, an
explicit raw definition budget, and a per-TU promotion budget.  It is a fair cold
bootstrap control, not a claim that this is the final production learner.
"""

from __future__ import annotations

import argparse
import collections
import csv
import dataclasses
import hashlib
import heapq
import json
import random
import time
from pathlib import Path
from typing import Iterable, Iterator, Sequence

import zstandard as zstd

from pretrained_superblocks import (
    ExactDecoder,
    ExactEncoder,
    StaticPackage,
    TuRegions,
    build_package,
    collect_candidates,
    context_runs,
    decode_key,
    encode_regions,
    expected_regions,
    get_varint,
    iter_tus_fast,
    materialization_keys,
    package_from_keys,
    phrase_candidates,
    put_varint,
)
from prior_root_copy import (
    ExactState as PriorRootState,
    PriorRootIndex,
    deserialize as deserialize_prior_root,
    serialize as serialize_prior_root,
)


MODEL_VERSION = 1
REFERENCE_MODEL_VERSION = 1
MIXED_DEFINITION_VERSION = 2


def clone_package(package: StaticPackage) -> StaticPackage:
    return package_from_keys(list(package.keys), package.definition_bytes)


def append_package_keys(package: StaticPackage, keys: Iterable[bytes]) -> None:
    touched: set[tuple[int, int, int]] = set()
    for key in keys:
        if key in package.ids:
            raise ValueError("duplicate exact phrase definition")
        decoded = decode_key(key)
        static_id = len(package.keys) + 1
        package.keys.append(key)
        package.ids[key] = static_id
        if decoded:
            first = decoded[0]
            package.by_first.setdefault(first, []).append((static_id, key, decoded))
            touched.add(first)
    for first in touched:
        package.by_first[first].sort(key=lambda value: (-len(value[2]), value[0]))


def serialize_key_batch(keys: Sequence[bytes]) -> bytes:
    out = bytearray(put_varint(MODEL_VERSION))
    out += put_varint(len(keys))
    for key in keys:
        # Validate the exact object before it can become decoder-visible state.
        decode_key(key)
        out += put_varint(len(key))
        out += key
    return bytes(out)


def deserialize_key_batch(raw: bytes) -> list[bytes]:
    version, offset = get_varint(raw, 0)
    if version != MODEL_VERSION:
        raise ValueError("unknown online phrase package version")
    count, offset = get_varint(raw, offset)
    keys: list[bytes] = []
    for _ in range(count):
        size, offset = get_varint(raw, offset)
        if offset + size > len(raw):
            raise ValueError("truncated online phrase definition")
        key = raw[offset:offset + size]
        offset += size
        decode_key(key)
        keys.append(key)
    if offset != len(raw):
        raise ValueError("trailing online phrase package bytes")
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate phrase within definition batch")
    return keys


def compress_frame(raw: bytes, level: int) -> bytes:
    return zstd.ZstdCompressor(level=level).compress(raw)


def decompress_frame(frame: bytes) -> bytes:
    return zstd.ZstdDecompressor().decompress(frame)


def package_frame(package: StaticPackage, level: int) -> tuple[bytes, bytes]:
    raw = serialize_key_batch(package.keys)
    frame = compress_frame(raw, level)
    decoded = decompress_frame(frame)
    if decoded != raw or serialize_key_batch(deserialize_key_batch(decoded)) != raw:
        raise ValueError("non-canonical exact phrase package")
    return raw, frame


class RegionIdStore:
    """Dense first-observation IDs independently reproduced at C and F."""

    def __init__(self) -> None:
        self.ids: dict[tuple[int, int, int], int] = {}
        self.values: list[tuple[int, int, int]] = [(0, 0, 0)]

    def observe(self, regions: Sequence[tuple[int, int, int]]) -> None:
        for region in regions:
            if region not in self.ids:
                self.ids[region] = len(self.values)
                self.values.append(region)


def put_signed_delta(value: int) -> bytes:
    encoded = value * 2 if value >= 0 else -value * 2 - 1
    return put_varint(encoded)


def get_signed_delta(data: bytes, offset: int) -> tuple[int, int]:
    encoded, offset = get_varint(data, offset)
    value = encoded // 2 if encoded % 2 == 0 else -(encoded // 2) - 1
    return value, offset


def serialize_reference_batch(keys: Sequence[bytes], store: RegionIdStore) -> bytes:
    out = bytearray(put_varint(REFERENCE_MODEL_VERSION))
    out += put_varint(len(keys))
    for key in keys:
        regions = decode_key(key)
        out += put_varint(len(regions))
        previous = 0
        for region in regions:
            region_id = store.ids.get(region)
            if region_id is None:
                raise ValueError("online phrase references a Region not yet observed")
            out += put_signed_delta(region_id - previous)
            previous = region_id
    return bytes(out)


def deserialize_reference_batch(raw: bytes, store: RegionIdStore) -> list[bytes]:
    version, offset = get_varint(raw, 0)
    if version != REFERENCE_MODEL_VERSION:
        raise ValueError("unknown Region-reference phrase package version")
    count, offset = get_varint(raw, offset)
    keys: list[bytes] = []
    for _ in range(count):
        region_count, offset = get_varint(raw, offset)
        previous = 0
        regions: list[tuple[int, int, int]] = []
        for _ in range(region_count):
            delta, offset = get_signed_delta(raw, offset)
            region_id = previous + delta
            if not 0 < region_id < len(store.values):
                raise ValueError("online phrase references an unknown dense Region ID")
            regions.append(store.values[region_id])
            previous = region_id
        keys.append(encode_regions(regions))
    if offset != len(raw):
        raise ValueError("trailing Region-reference phrase bytes")
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate phrase within Region-reference batch")
    return keys


def serialize_mixed_definition_batch(
    keys: Sequence[bytes],
    store: RegionIdStore,
) -> bytes:
    """Use dense references when possible and exact keys for unseen Regions."""
    out = bytearray(put_varint(MIXED_DEFINITION_VERSION))
    out += put_varint(len(keys))
    for key in keys:
        regions = decode_key(key)
        if all(region in store.ids for region in regions):
            out.append(0)
            out += put_varint(len(regions))
            previous = 0
            for region in regions:
                region_id = store.ids[region]
                out += put_signed_delta(region_id - previous)
                previous = region_id
        else:
            out.append(1)
            out += put_varint(len(key))
            out += key
    return bytes(out)


def deserialize_mixed_definition_batch(
    raw: bytes,
    store: RegionIdStore,
) -> list[bytes]:
    version, offset = get_varint(raw, 0)
    if version != MIXED_DEFINITION_VERSION:
        raise ValueError("unknown mixed phrase-definition version")
    count, offset = get_varint(raw, offset)
    keys: list[bytes] = []
    for _ in range(count):
        if offset >= len(raw):
            raise ValueError("truncated mixed phrase definition")
        representation = raw[offset]
        offset += 1
        if representation == 0:
            region_count, offset = get_varint(raw, offset)
            previous = 0
            regions: list[tuple[int, int, int]] = []
            for _ in range(region_count):
                delta, offset = get_signed_delta(raw, offset)
                region_id = previous + delta
                if not 0 < region_id < len(store.values):
                    raise ValueError("mixed phrase references an unknown Region ID")
                regions.append(store.values[region_id])
                previous = region_id
            keys.append(encode_regions(regions))
        elif representation == 1:
            size, offset = get_varint(raw, offset)
            if offset + size > len(raw):
                raise ValueError("truncated exact phrase definition")
            key = raw[offset:offset + size]
            offset += size
            decode_key(key)
            keys.append(key)
        else:
            raise ValueError("unknown mixed phrase representation")
    if offset != len(raw):
        raise ValueError("trailing mixed phrase-definition bytes")
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate phrase within mixed definition batch")
    return keys


def deserialize_definition_batch(
    raw: bytes,
    store: RegionIdStore,
) -> list[bytes]:
    """Dispatch solely from the definition block's on-wire version."""
    version, _ = get_varint(raw, 0)
    if version == REFERENCE_MODEL_VERSION:
        return deserialize_reference_batch(raw, store)
    if version == MIXED_DEFINITION_VERSION:
        return deserialize_mixed_definition_batch(raw, store)
    raise ValueError("unknown phrase-definition version")


def clone_encoder(source: ExactEncoder, package: StaticPackage) -> ExactEncoder:
    clone = ExactEncoder(package, {})
    clone.dynamic = dict(source.dynamic)
    return clone


def overlapping_phrases(
    regions: Sequence[tuple[int, int, int]],
    lengths: Sequence[int],
) -> Iterator[bytes]:
    """Emit deterministic half-overlapping windows over one Region sequence."""
    for length in lengths:
        if length > len(regions):
            continue
        stride = max(1, length // 2)
        starts = list(range(0, len(regions) - length + 1, stride))
        final = len(regions) - length
        if not starts or starts[-1] != final:
            starts.append(final)
        for start in starts:
            yield encode_regions(regions[start:start + length])


def scoped_phrase_candidates(
    tu: TuRegions,
    lengths: Sequence[int],
    phrase_scope: str,
) -> Iterator[bytes]:
    """Add cross-context candidates without removing the stable run-local set."""
    yield from phrase_candidates(tu, lengths)
    if phrase_scope == "run":
        return
    if phrase_scope != "run-and-tu":
        raise ValueError(f"unknown phrase scope: {phrase_scope}")
    yield from overlapping_phrases(expected_regions(tu), lengths)


def scoped_materialization_keys(
    tu: TuRegions,
    package: StaticPackage,
    phrase_scope: str,
    atomize_raw: bool = False,
) -> list[bytes]:
    """Greedily match exact phrases across context boundaries when requested."""
    if phrase_scope == "run" and not atomize_raw:
        return materialization_keys(tu, package)
    if phrase_scope not in ("run", "run-and-tu"):
        raise ValueError(f"unknown phrase scope: {phrase_scope}")

    out: list[bytes] = []

    def append_sequence(regions: Sequence[tuple[int, int, int]]) -> None:
        raw: list[tuple[int, int, int]] = []

        def flush_raw() -> None:
            if not raw:
                return
            if atomize_raw:
                out.extend(encode_regions((region,)) for region in raw)
            else:
                out.append(encode_regions(raw))
            raw.clear()

        offset = 0
        while offset < len(regions):
            selected: bytes | None = None
            selected_length = 0
            for _, key, phrase in package.by_first.get(regions[offset], ()):
                length = len(phrase)
                if (
                    offset + length <= len(regions)
                    and regions[offset:offset + length] == phrase
                ):
                    selected = key
                    selected_length = length
                    break
            if selected is None:
                raw.append(regions[offset])
                offset += 1
                continue
            flush_raw()
            out.append(selected)
            offset += selected_length
        flush_raw()

    if phrase_scope == "run-and-tu":
        append_sequence(expected_regions(tu))
    else:
        for regions in context_runs(tu):
            append_sequence(regions)
    return out


def canonical_atom_frame(
    keys: Sequence[bytes],
    package: StaticPackage,
    store: RegionIdStore,
) -> bytes:
    """Encode raw atoms against the causal dense Region store without mutating it."""
    out = bytearray(put_varint(len(keys)))
    transient: dict[tuple[int, int, int], int] = {}
    for key in keys:
        static_id = package.ids.get(key, 0)
        if static_id:
            out.append(ExactEncoder.STATIC_ID)
            out += put_varint(static_id)
            continue
        regions = decode_key(key)
        if len(regions) != 1:
            raise ValueError("canonical fallback must contain exactly one Region")
        region = regions[0]
        dynamic_id = store.ids.get(region) or transient.get(region)
        if dynamic_id is None:
            dynamic_id = len(store.values) + len(transient)
            transient[region] = dynamic_id
            out.append(ExactEncoder.RAW_DEFINE)
            out += put_varint(dynamic_id)
            out += put_varint(len(key))
            out += key
        else:
            out.append(ExactEncoder.DYNAMIC_REF)
            out += put_varint(dynamic_id)
    return bytes(out)


def decode_canonical_atom_frame(
    data: bytes,
    package: StaticPackage,
    store: RegionIdStore,
) -> list[tuple[int, int, int]]:
    """Decode one frame using persistent Region IDs plus frame-local new atoms."""
    count, offset = get_varint(data, 0)
    persistent_count = len(store.values) - 1
    transient: list[tuple[int, int, int]] = []
    transient_seen: set[tuple[int, int, int]] = set()
    out: list[tuple[int, int, int]] = []
    for _ in range(count):
        if offset >= len(data):
            raise ValueError("truncated canonical atom opcode")
        op = data[offset]
        offset += 1
        if op == ExactEncoder.RAW_DEFINE:
            dynamic_id, offset = get_varint(data, offset)
            size, offset = get_varint(data, offset)
            expected_id = persistent_count + len(transient) + 1
            if dynamic_id != expected_id or offset + size > len(data):
                raise ValueError("bad frame-local Region definition")
            regions = decode_key(data[offset:offset + size])
            offset += size
            if (
                len(regions) != 1
                or regions[0] in store.ids
                or regions[0] in transient_seen
            ):
                raise ValueError("non-canonical frame-local Region definition")
            transient.append(regions[0])
            transient_seen.add(regions[0])
            out.append(regions[0])
        elif op == ExactEncoder.DYNAMIC_REF:
            dynamic_id, offset = get_varint(data, offset)
            if 0 < dynamic_id <= persistent_count:
                out.append(store.values[dynamic_id])
                continue
            transient_id = dynamic_id - persistent_count
            if not 0 < transient_id <= len(transient):
                raise ValueError("unknown canonical Region ID")
            out.append(transient[transient_id - 1])
        elif op == ExactEncoder.STATIC_ID:
            static_id, offset = get_varint(data, offset)
            if not 0 < static_id <= len(package.keys):
                raise ValueError("unknown static phrase ID")
            out.extend(decode_key(package.keys[static_id - 1]))
        else:
            raise ValueError("unsupported canonical atom opcode")
    if offset != len(data):
        raise ValueError("trailing canonical atom bytes")
    return out


def context_run_lengths(tu: TuRegions) -> list[int]:
    """Return the exact Region count of every consecutive semantic-context run."""
    lengths: list[int] = []
    offset = 0
    while offset < len(tu.regions):
        context = tu.regions[offset][2]
        end = offset + 1
        while end < len(tu.regions) and tu.regions[end][2] == context:
            end += 1
        lengths.append(end - offset)
        offset = end
    return lengths


def serialize_context_runs(lengths: Sequence[int]) -> bytes:
    out = bytearray(put_varint(len(lengths)))
    for length in lengths:
        if length <= 0:
            raise ValueError("context run must contain at least one Region")
        out += put_varint(length)
    return bytes(out)


def deserialize_context_runs(raw: bytes, region_count: int) -> list[int]:
    count, offset = get_varint(raw, 0)
    lengths: list[int] = []
    for _ in range(count):
        length, offset = get_varint(raw, offset)
        if length <= 0:
            raise ValueError("empty context run")
        lengths.append(length)
    if offset != len(raw) or sum(lengths) != region_count:
        raise ValueError("context runs do not cover the reconstructed TU")
    return lengths


def tu_with_context_runs(
    regions: Sequence[tuple[int, int, int]],
    lengths: Sequence[int],
) -> TuRegions:
    contextual: list[tuple[int, int, int, int]] = []
    offset = 0
    for context, length in enumerate(lengths, 1):
        for hash1, hash2, raw_length in regions[offset:offset + length]:
            contextual.append((hash1, hash2, context, raw_length))
        offset += length
    if offset != len(regions):
        raise ValueError("context runs do not match Region count")
    return TuRegions(0, 0, contextual)


def advance_canonical_dynamic(
    previous: dict[bytes, int],
    keys: Sequence[bytes],
    static: StaticPackage,
) -> dict[bytes, int]:
    """Apply the fixed canonical parse without retaining frame-local choices."""
    dynamic = dict(previous)
    for key in keys:
        if key not in static.ids and key not in dynamic:
            dynamic[key] = len(dynamic) + 1
    return dynamic


def dense_decoder_dynamic(dynamic: dict[bytes, int]) -> list[bytes]:
    values = [b""] * (len(dynamic) + 1)
    for key, dynamic_id in dynamic.items():
        if not 0 < dynamic_id < len(values) or values[dynamic_id]:
            raise ValueError("dynamic dictionary IDs are not dense and unique")
        values[dynamic_id] = key
    if any(not key for key in values[1:]):
        raise ValueError("dynamic dictionary ID gap")
    return values


def coverage_sequence_keys(
    regions: Sequence[tuple[int, int, int]],
    package: StaticPackage,
) -> list[bytes]:
    """Choose non-overlapping phrases by maximum uncompressed byte coverage."""
    count = len(regions)
    best = [0] * (count + 1)
    choices: list[tuple[bytes, int] | None] = [None] * count
    for offset in range(count - 1, -1, -1):
        best[offset] = best[offset + 1]
        for static_id, key, phrase in package.by_first.get(regions[offset], ()):
            length = len(phrase)
            if (
                offset + length > count
                or regions[offset:offset + length] != phrase
            ):
                continue
            reference_bytes = 1 + len(put_varint(static_id))
            score = 20 * length - reference_bytes + best[offset + length]
            if score > best[offset]:
                best[offset] = score
                choices[offset] = (key, length)

    out: list[bytes] = []
    raw: list[tuple[int, int, int]] = []
    offset = 0
    while offset < count:
        selected = choices[offset]
        if selected is None:
            raw.append(regions[offset])
            offset += 1
            continue
        if raw:
            out.append(encode_regions(raw))
            raw.clear()
        key, length = selected
        out.append(key)
        offset += length
    if raw:
        out.append(encode_regions(raw))
    return out


def coverage_materialization_keys(
    tu: TuRegions,
    package: StaticPackage,
    phrase_scope: str,
) -> list[bytes]:
    """Run the coverage parse within runs or across the complete TU."""
    if not package.keys:
        return materialization_keys(tu, package)
    if phrase_scope == "run-and-tu":
        return coverage_sequence_keys(expected_regions(tu), package)
    if phrase_scope != "run":
        raise ValueError(f"unknown phrase scope: {phrase_scope}")
    out: list[bytes] = []
    for regions in context_runs(tu):
        out.extend(coverage_sequence_keys(regions, package))
    return out


def static_cross_materialization_keys(
    base_keys: Sequence[bytes],
    package: StaticPackage,
) -> list[bytes]:
    """Replace adjacent static keys without changing raw-key state transitions."""
    decoded = [decode_key(key) for key in base_keys]
    out: list[bytes] = []
    key_offset = 0
    while key_offset < len(base_keys):
        if base_keys[key_offset] not in package.ids or not decoded[key_offset]:
            out.append(base_keys[key_offset])
            key_offset += 1
            continue

        regions: list[tuple[int, int, int]] = []
        boundary_to_keys: dict[int, int] = {}
        end = key_offset
        while end < len(base_keys) and base_keys[end] in package.ids:
            regions.extend(decoded[end])
            boundary_to_keys[len(regions)] = end - key_offset + 1
            end += 1

        selected: bytes | None = None
        consumed = 0
        for _, key, phrase in package.by_first.get(regions[0], ()):
            key_count = boundary_to_keys.get(len(phrase), 0)
            if key_count < 2 or regions[:len(phrase)] != phrase:
                continue
            selected = key
            consumed = key_count
            break
        if selected is None:
            out.append(base_keys[key_offset])
            key_offset += 1
            continue
        out.append(selected)
        key_offset += consumed
    return out


class BoundedDocumentCounts:
    """Deterministic bounded document-frequency table with conservative counts."""

    def __init__(self, capacity: int):
        if capacity <= 0:
            raise ValueError("candidate capacity must be positive")
        self.capacity = capacity
        self.counts: dict[bytes, int] = {}
        self.error: dict[bytes, int] = {}
        self.heap: list[tuple[int, int, bytes]] = []
        self.serial = 0
        self.key_bytes = 0

    def add(self, key: bytes) -> int:
        self.serial += 1
        if key in self.counts:
            self.counts[key] += 1
            heapq.heappush(self.heap, (self.counts[key], self.serial, key))
            self._compact_heap()
            return self.counts[key] - self.error[key]
        if len(self.counts) < self.capacity:
            self.counts[key] = 1
            self.error[key] = 0
            self.key_bytes += len(key)
            heapq.heappush(self.heap, (1, self.serial, key))
            return 1
        while self.heap:
            count, _, victim = heapq.heappop(self.heap)
            if self.counts.get(victim) == count:
                break
        else:
            raise RuntimeError("empty bounded-count heap")
        self.key_bytes -= len(victim)
        del self.counts[victim]
        del self.error[victim]
        self.counts[key] = count + 1
        self.error[key] = count
        self.key_bytes += len(key)
        heapq.heappush(self.heap, (count + 1, self.serial, key))
        return 1

    def forget(self, key: bytes) -> None:
        if key in self.counts:
            self.key_bytes -= len(key)
            del self.counts[key]
            del self.error[key]

    def _compact_heap(self) -> None:
        if len(self.heap) <= max(10_000, 3 * self.capacity):
            return
        self.heap = [
            (count, i, key)
            for i, (key, count) in enumerate(self.counts.items())
        ]
        heapq.heapify(self.heap)

    @property
    def logical_bytes(self) -> int:
        # Exact key storage plus two u32 counters per retained candidate.  Python
        # allocator overhead is reported separately as process RSS by the runner.
        return self.key_bytes + 8 * len(self.counts)


class CausalPhraseLearner:
    def __init__(
        self,
        initial: StaticPackage,
        phrase_lengths: Sequence[int],
        threshold: int,
        online_budget: int,
        per_tu_budget: int,
        candidate_capacity: int,
        maximum_key_bytes: int,
        budget_basis: str,
        phrase_scope: str,
    ):
        self.vocabulary = clone_package(initial)
        self.initial_assets = len(initial.keys)
        self.phrase_lengths = tuple(phrase_lengths)
        self.threshold = threshold
        self.online_budget = online_budget
        self.per_tu_budget = per_tu_budget
        self.maximum_key_bytes = maximum_key_bytes
        self.budget_basis = budget_basis
        self.phrase_scope = phrase_scope
        self.counts = BoundedDocumentCounts(candidate_capacity)
        self.promoted_raw_bytes = 0
        self.promoted_assets = 0
        self.candidate_observations = 0

    def record_size(self, key: bytes) -> int:
        if self.budget_basis == "raw":
            return len(put_varint(len(key))) + len(key)
        region_count, _ = get_varint(key, 0)
        return len(put_varint(region_count)) + 4 * region_count

    def observe(self, tu: TuRegions) -> list[bytes]:
        """Observe TU after encoding, then append a bounded immutable promotion set."""
        if self.promoted_raw_bytes >= self.online_budget:
            return []
        seen: set[bytes] = set()
        eligible: list[tuple[float, int, int, bytes]] = []
        for key in scoped_phrase_candidates(
            tu,
            self.phrase_lengths,
            self.phrase_scope,
        ):
            if key in seen:
                continue
            seen.add(key)
            if key in self.vocabulary.ids or len(key) > self.maximum_key_bytes:
                continue
            self.candidate_observations += 1
            lower = self.counts.add(key)
            if lower < self.threshold:
                continue
            record = self.record_size(key)
            reference = 3
            gain = lower * max(1, len(key) - reference) - record
            if gain <= 0:
                continue
            roi = lower * max(1, len(key) - reference) / record
            eligible.append((-roi, -gain, -len(key), key))

        eligible.sort()
        remaining_total = self.online_budget - self.promoted_raw_bytes
        remaining_tu = min(self.per_tu_budget, remaining_total)
        promoted: list[bytes] = []
        for _, _, _, key in eligible:
            record = self.record_size(key)
            if record > remaining_tu:
                continue
            promoted.append(key)
            remaining_tu -= record
            self.promoted_raw_bytes += record
            self.promoted_assets += 1
            self.counts.forget(key)
            if remaining_tu == 0:
                break
        append_package_keys(self.vocabulary, promoted)
        return promoted

    @property
    def logical_state_bytes(self) -> int:
        return self.promoted_raw_bytes + self.counts.logical_bytes


def summarize_learning_gate(
    curve: Sequence[dict],
    initial_wire_bytes: int,
    total_raw_bytes: int,
    target_ratio: float = 200.0,
) -> dict:
    """Compute the issue #16 trailing-window H200 gate and literal C50."""
    if not curve or total_raw_bytes <= 0:
        return {
            "target_ratio": target_ratio,
            "window_raw_fraction": 0.05,
            "sustain_raw_fraction": 0.10,
            "minimum_window_tus": 64,
            "effective_minimum_window_tus": 0,
            "c50_tu": None,
            "c50_ratio": None,
            "h200_tu": None,
            "h200_fraction": None,
        }

    minimum_tus = min(64, len(curve))
    window_raw_target = total_raw_bytes * 0.05
    sustain_raw_target = total_raw_bytes * 0.10
    prefix_raw = [0]
    prefix_wire = [0]
    for point in curve:
        prefix_raw.append(prefix_raw[-1] + point["raw_bytes"])
        prefix_wire.append(prefix_wire[-1] + point["wire_bytes"])

    window_ratios: list[float | None] = []
    start = 0
    for end in range(1, len(curve) + 1):
        while (
            end - (start + 1) >= minimum_tus
            and prefix_raw[end] - prefix_raw[start + 1] >= window_raw_target
        ):
            start += 1
        if end - start < minimum_tus:
            window_ratios.append(None)
            continue
        wire = prefix_wire[end] - prefix_wire[start]
        if start == 0:
            wire += initial_wire_bytes
        window_ratios.append((prefix_raw[end] - prefix_raw[start]) / max(1, wire))

    c50_index = next(
        (i for i, raw in enumerate(prefix_raw[1:]) if raw >= total_raw_bytes * 0.50),
        len(curve) - 1,
    )
    h200_index: int | None = None
    for candidate, ratio in enumerate(window_ratios):
        if ratio is None or ratio < target_ratio:
            continue
        sustain_raw_end = prefix_raw[candidate + 1] + sustain_raw_target
        final = next(
            (
                i
                for i in range(candidate, len(curve))
                if prefix_raw[i + 1] >= sustain_raw_end
            ),
            None,
        )
        if final is None:
            continue
        if all(
            value is not None and value >= target_ratio
            for value in window_ratios[candidate:final + 1]
        ):
            h200_index = candidate
            break

    c50_point = curve[c50_index]
    return {
        "target_ratio": target_ratio,
        "window_raw_fraction": 0.05,
        "sustain_raw_fraction": 0.10,
        "minimum_window_tus": 64,
        "effective_minimum_window_tus": minimum_tus,
        "c50_tu": c50_index + 1,
        "c50_raw_fraction": c50_point["cumulative_raw_bytes"] / total_raw_bytes,
        "c50_ratio": c50_point["cumulative_charged_ratio"],
        "h200_tu": None if h200_index is None else h200_index + 1,
        "h200_fraction": (
            None
            if h200_index is None
            else curve[h200_index]["cumulative_raw_bytes"] / total_raw_bytes
        ),
        "final_window_ratio": window_ratios[-1],
    }


@dataclasses.dataclass(slots=True)
class OnlineResult:
    name: str
    initial_assets: int
    initial_model_raw_bytes: int
    initial_model_wire_bytes: int
    online_budget: int
    promotion_threshold: int
    per_tu_budget: int
    candidate_capacity: int
    publication: str
    budget_basis: str
    pretrained_mode: str
    phrase_scope: str
    parse_policy: str
    cross_probe_tus: int
    cross_probe_min_savings: int
    cross_probe_live: bool
    prior_root_copy: bool
    root_copy_seed_length: int
    root_copy_candidates: int
    root_copy_index_stride: int
    cross_probe_savings: int = 0
    cross_enabled: bool = False
    root_copy_selected_tus: int = 0
    root_copy_copies: int = 0
    root_copy_copied_regions: int = 0
    root_copy_receiver_state_bytes: int = 0
    root_copy_encoder_state_bytes: int = 0
    raw_bytes: int = 0
    payload_wire_bytes: int = 0
    definition_wire_bytes: int = 0
    definition_uncompressed_bytes: int = 0
    context_wire_bytes: int = 0
    selector_wire_bytes: int = 0
    online_definition_raw_bytes: int = 0
    online_promoted_assets: int = 0
    online_published_assets: int = 0
    online_selected_tus: int = 0
    candidate_observations: int = 0
    final_logical_state_bytes: int = 0
    seed_assets: int = 0
    seed_model_raw_bytes: int = 0
    seed_model_compressed_bytes: int = 0
    seed_published_assets: int = 0
    encode_seconds: float = 0.0
    update_seconds: float = 0.0
    decode_seconds: float = 0.0
    tus: int = 0
    exact: bool = True
    per_tu_curve: list[dict] = dataclasses.field(default_factory=list)

    def as_dict(self) -> dict:
        charged = (
            self.initial_model_wire_bytes
            + self.payload_wire_bytes
            + self.definition_wire_bytes
            + self.context_wire_bytes
            + self.selector_wire_bytes
        )
        return {
            **dataclasses.asdict(self),
            "charged_wire_bytes": charged,
            "charged_ratio": self.raw_bytes / max(1, charged),
            "encode_effective_GBps": self.raw_bytes / max(1e-12, self.encode_seconds) / 1e9,
            "decode_effective_GBps": self.raw_bytes / max(1e-12, self.decode_seconds) / 1e9,
            "learning_gate": summarize_learning_gate(
                self.per_tu_curve,
                self.initial_model_wire_bytes,
                self.raw_bytes,
            ),
        }


def evaluate_online(
    target_tus: Sequence[TuRegions],
    name: str,
    initial: StaticPackage,
    initial_frame: bytes,
    level: int,
    phrase_lengths: Sequence[int],
    phrase_scope: str,
    parse_policy: str,
    cross_probe_tus: int,
    cross_probe_min_savings: int,
    cross_probe_live: bool,
    threshold: int,
    online_budget: int,
    per_tu_budget: int,
    candidate_capacity: int,
    maximum_key_bytes: int,
    publication: str,
    budget_basis: str,
    seed_only: bool = False,
    prior_root_copy: bool = False,
    root_copy_seed_length: int = 4,
    root_copy_candidates: int = 4,
    root_copy_index_stride: int = 1,
) -> OnlineResult:
    if cross_probe_tus < 0 or cross_probe_min_savings < 0:
        raise ValueError("cross probe controls must be nonnegative")
    if cross_probe_live and not cross_probe_tus:
        raise ValueError("live cross probing requires a positive probe length")
    if cross_probe_tus and phrase_scope != "run-vs-tu-match":
        raise ValueError("cross probing requires run-vs-tu-match")
    if (
        phrase_scope in (
            "run-vs-tu-match-canonical",
            "run-vs-tu-match-hybrid",
        )
        and parse_policy != "greedy"
    ):
        raise ValueError("canonical atom matching currently requires greedy parsing")
    shadow_probe_wire: int | None = None
    probe_limit = min(cross_probe_tus, len(target_tus))
    if cross_probe_live and probe_limit:
        shadow = evaluate_online(
            target_tus[:probe_limit],
            f"{name}-probe-shadow",
            initial,
            initial_frame,
            level,
            phrase_lengths,
            "run",
            "greedy",
            0,
            0,
            False,
            threshold,
            online_budget,
            per_tu_budget,
            candidate_capacity,
            maximum_key_bytes,
            publication,
            budget_basis,
            seed_only,
        )
        shadow_probe_wire = shadow.as_dict()["charged_wire_bytes"]
    learner_scope = (
        "run"
        if phrase_scope in (
            "run-vs-tu-match",
            "run-vs-tu-match-canonical",
            "run-vs-tu-match-context",
            "run-vs-tu-match-hybrid",
            "run-plus-static-cross",
        )
        else phrase_scope
    )
    if phrase_scope in (
        "run-vs-tu-match",
        "run-vs-tu-match-canonical",
        "run-vs-tu-match-context",
        "run-vs-tu-match-hybrid",
    ):
        match_scopes = ("run", "run-and-tu")
    elif phrase_scope == "run-plus-static-cross":
        match_scopes = ("run",)
    else:
        match_scopes = (phrase_scope,)
    state_identical_cross = phrase_scope == "run-plus-static-cross"
    canonical_cross = (
        phrase_scope == "run-vs-tu-match-canonical" and bool(online_budget)
    )
    canonical_context = (
        phrase_scope in (
            "run-vs-tu-match-context",
            "run-vs-tu-match-hybrid",
        )
        and bool(online_budget)
    )
    if prior_root_copy and not canonical_context:
        raise ValueError("prior-root copy requires canonical context state")
    if (
        root_copy_seed_length < 2
        or root_copy_candidates <= 0
        or root_copy_index_stride <= 0
    ):
        raise ValueError("prior-root copy controls must be positive")
    hybrid_cross = (
        phrase_scope == "run-vs-tu-match-hybrid" and bool(online_budget)
    )
    initial_raw = decompress_frame(initial_frame)
    if deserialize_key_batch(initial_raw) != initial.keys:
        raise ValueError("supplied initial model frame differs from encoder package")
    installed_initial = package_from_keys([], 0) if seed_only else initial
    frozen_baseline = installed_initial
    installed_encoder = clone_package(installed_initial)
    installed_decoder = package_from_keys([], 0)
    if not seed_only:
        append_package_keys(
            installed_decoder,
            deserialize_key_batch(decompress_frame(initial_frame)),
        )
    if installed_decoder.keys != installed_encoder.keys:
        raise ValueError("initial receiver package differs from encoder package")

    encoder = ExactEncoder(installed_encoder, {})
    decoder = ExactDecoder(installed_decoder, {})
    encoder_regions = RegionIdStore()
    decoder_regions = RegionIdStore()
    prior_root_encoder = PriorRootState()
    prior_root_decoder = PriorRootState()
    prior_root_index = PriorRootIndex(
        root_copy_seed_length,
        root_copy_candidates,
        root_copy_index_stride,
    )
    learner = CausalPhraseLearner(
        initial,
        phrase_lengths,
        threshold,
        online_budget,
        per_tu_budget,
        candidate_capacity,
        maximum_key_bytes,
        budget_basis,
        learner_scope,
    )
    result = OnlineResult(
        name=name,
        initial_assets=len(installed_initial.keys),
        initial_model_raw_bytes=0 if seed_only else len(initial_raw),
        initial_model_wire_bytes=(
            0 if seed_only else len(initial_frame) + 4 if initial.keys else 0
        ),
        online_budget=online_budget,
        promotion_threshold=threshold,
        per_tu_budget=per_tu_budget,
        candidate_capacity=candidate_capacity,
        publication=publication,
        budget_basis=budget_basis,
        pretrained_mode="seed-only" if seed_only else "installed",
        phrase_scope=phrase_scope,
        parse_policy=parse_policy,
        cross_probe_tus=cross_probe_tus,
        cross_probe_min_savings=cross_probe_min_savings,
        cross_probe_live=cross_probe_live,
        prior_root_copy=prior_root_copy,
        root_copy_seed_length=root_copy_seed_length,
        root_copy_candidates=root_copy_candidates,
        root_copy_index_stride=root_copy_index_stride,
        cross_enabled=cross_probe_tus == 0 or cross_probe_live,
        seed_assets=len(initial.keys) if seed_only else 0,
        seed_model_raw_bytes=len(initial_raw) if seed_only else 0,
        seed_model_compressed_bytes=len(initial_frame) if seed_only else 0,
    )
    compressor = zstd.ZstdCompressor(level=level)
    decompressor = zstd.ZstdDecompressor()
    cumulative_wire = result.initial_model_wire_bytes

    for ordinal, tu in enumerate(target_tus, 1):
        begin = time.monotonic()
        expected = expected_regions(tu)
        selector_wire = 1 if online_budget else 0
        context_wire = 0
        context_frame = b""
        sender_context_lengths: list[int] = []
        receiver_context_tu: TuRegions | None = None
        context_encoder_before = encoder.dynamic if canonical_context else {}
        context_decoder_before = (
            {key: dynamic_id for dynamic_id, key in enumerate(decoder.dynamic) if dynamic_id}
            if canonical_context
            else {}
        )
        if context_decoder_before != context_encoder_before:
            raise ValueError("canonical context dictionaries diverged before TU")
        if canonical_context:
            sender_context_lengths = context_run_lengths(tu)
            context_frame = compressor.compress(
                serialize_context_runs(sender_context_lengths)
            )
            context_wire = len(context_frame) + 4
        definition_wire = 0
        definition_raw = b""
        used_online = False
        root_copy_selected = False
        root_copy_copies = 0
        root_copy_copied_regions = 0
        root_copy_new_regions: list[tuple[int, int, int]] = []
        root_copy_recovered: list[tuple[int, int, int]] = []
        root_copy_received: list[tuple[int, int, int]] = []
        selected_representation = (
            "atom"
            if canonical_cross
            else "context"
            if canonical_context
            else "chosen"
        )

        if online_budget:
            # Compare both representations from exactly the same prior dynamic
            # dictionary.  The baseline retains the frozen initial vocabulary;
            # the online candidate may use all causally promoted phrases.
            baseline_keys = (
                scoped_materialization_keys(
                    tu,
                    frozen_baseline,
                    "run",
                    atomize_raw=True,
                )
                if canonical_cross
                else materialization_keys(tu, frozen_baseline)
            )
            baseline_encoder = clone_encoder(encoder, installed_encoder)
            baseline_payload = (
                canonical_atom_frame(
                    baseline_keys,
                    installed_encoder,
                    encoder_regions,
                )
                if canonical_cross
                else baseline_encoder.frame(baseline_keys)
            )
            baseline_frame = compressor.compress(baseline_payload)
            baseline_wire = len(baseline_frame) + 4 + selector_wire
            best_wire = baseline_wire
            best_encoder = baseline_encoder
            best_package = installed_encoder
            best_payload = baseline_payload
            best_frame = baseline_frame
            best_definition_raw = b""
            best_definition_frame = b""
            best_definitions: list[bytes] = []
            best_representation = selected_representation

            parse_materializations: list[tuple[str, str]] = [
                ("greedy", match_scope) for match_scope in match_scopes
            ]
            if parse_policy == "greedy-plus-coverage":
                parse_materializations.extend(
                    ("coverage", match_scope) for match_scope in match_scopes
                )
            elif parse_policy != "greedy":
                raise ValueError(f"unknown parse policy: {parse_policy}")

            representations = (
                ("context", "atom")
                if hybrid_cross
                else (selected_representation,)
            )
            materializations = (
                (materializer, match_scope, representation)
                for materializer, match_scope in parse_materializations
                for representation in representations
            )
            for materializer, match_scope, representation in materializations:
                if materializer == "greedy":
                    online_keys = scoped_materialization_keys(
                        tu,
                        learner.vocabulary,
                        match_scope,
                        atomize_raw=representation == "atom",
                    )
                else:
                    online_keys = coverage_materialization_keys(
                        tu,
                        learner.vocabulary,
                        match_scope,
                    )
                new_definitions: list[bytes] = []
                if publication == "first-use":
                    selected: set[bytes] = set()
                    for key in online_keys:
                        if (
                            key in learner.vocabulary.ids
                            and key not in installed_encoder.ids
                            and key not in selected
                        ):
                            selected.add(key)
                            new_definitions.append(key)

                candidate_package = clone_package(installed_encoder)
                append_package_keys(candidate_package, new_definitions)
                online_encoder = clone_encoder(encoder, candidate_package)
                online_payload = (
                    canonical_atom_frame(
                        online_keys,
                        candidate_package,
                        encoder_regions,
                    )
                    if representation == "atom"
                    else online_encoder.frame(online_keys)
                )
                online_frame = compressor.compress(online_payload)
                candidate_definition_raw = (
                    serialize_mixed_definition_batch(
                        new_definitions,
                        encoder_regions,
                    )
                    if seed_only
                    else serialize_reference_batch(
                        new_definitions,
                        encoder_regions,
                    )
                )
                candidate_definition_frame = (
                    compressor.compress(candidate_definition_raw)
                    if new_definitions
                    else b""
                )
                candidate_definition_wire = (
                    len(candidate_definition_frame) + 4
                    if new_definitions
                    else 0
                )
                online_wire = (
                    len(online_frame)
                    + 4
                    + candidate_definition_wire
                    + selector_wire
                )
                allow_candidate = True
                if (
                    phrase_scope == "run-vs-tu-match"
                    and match_scope == "run-and-tu"
                    and cross_probe_tus
                ):
                    if cross_probe_live:
                        allow_candidate = (
                            ordinal <= cross_probe_tus or result.cross_enabled
                        )
                    elif ordinal <= cross_probe_tus:
                        result.cross_probe_savings += best_wire - online_wire
                        allow_candidate = False
                        if ordinal == cross_probe_tus:
                            result.cross_enabled = (
                                result.cross_probe_savings
                                >= cross_probe_min_savings
                            )
                    else:
                        allow_candidate = result.cross_enabled
                if allow_candidate and online_wire < best_wire:
                    used_online = True
                    best_wire = online_wire
                    best_encoder = online_encoder
                    best_package = candidate_package
                    best_payload = online_payload
                    best_frame = online_frame
                    best_definition_raw = candidate_definition_raw
                    best_definition_frame = candidate_definition_frame
                    best_definitions = new_definitions
                    best_representation = representation

                if (
                    state_identical_cross
                    and representation == "chosen"
                    and materializer == "greedy"
                    and match_scope == "run"
                    and online_wire < baseline_wire
                ):
                    cross_keys = static_cross_materialization_keys(
                        online_keys,
                        candidate_package,
                    )
                    cross_encoder = clone_encoder(encoder, candidate_package)
                    cross_payload = cross_encoder.frame(cross_keys)
                    if cross_encoder.dynamic != online_encoder.dynamic:
                        raise ValueError(
                            "static cross parse changed dynamic encoder state"
                        )
                    cross_frame = compressor.compress(cross_payload)
                    cross_wire = (
                        len(cross_frame)
                        + 4
                        + candidate_definition_wire
                        + selector_wire
                    )
                    if cross_wire < best_wire:
                        used_online = True
                        best_wire = cross_wire
                        best_encoder = cross_encoder
                        best_package = candidate_package
                        best_payload = cross_payload
                        best_frame = cross_frame
                        best_definition_raw = candidate_definition_raw
                        best_definition_frame = candidate_definition_frame
                        best_definitions = new_definitions
                        best_representation = representation

            if prior_root_copy:
                if prior_root_encoder.ids != encoder_regions.ids:
                    raise ValueError("prior-root and dense Region IDs diverged before TU")
                (
                    root_copy_payload,
                    root_copy_new_regions,
                    root_copy_copies,
                    root_copy_copied_regions,
                ) = serialize_prior_root(
                    expected,
                    prior_root_encoder,
                    prior_root_index,
                    "greedy",
                )
                root_copy_frame = compressor.compress(root_copy_payload)
                root_copy_recovered_raw = decompressor.decompress(root_copy_frame)
                if root_copy_recovered_raw != root_copy_payload:
                    raise ValueError("prior-root frame differs after decompression")
                root_copy_recovered, root_copy_received = deserialize_prior_root(
                    root_copy_recovered_raw,
                    prior_root_decoder,
                )
                if (
                    root_copy_recovered != expected
                    or root_copy_received != root_copy_new_regions
                ):
                    raise ValueError("prior-root candidate differs after decode")
                root_copy_wire = len(root_copy_frame) + 4 + selector_wire
                if root_copy_wire < best_wire:
                    used_online = True
                    root_copy_selected = True
                    best_wire = root_copy_wire
                    best_encoder = clone_encoder(encoder, installed_encoder)
                    best_package = installed_encoder
                    best_payload = root_copy_payload
                    best_frame = root_copy_frame
                    best_definition_raw = b""
                    best_definition_frame = b""
                    best_definitions = []
                    best_representation = "prior-root"

            encoder = best_encoder
            payload = best_payload
            payload_frame = best_frame
            selected_representation = best_representation
            if used_online:
                installed_encoder = best_package
                definition_raw = best_definition_raw
                definition_wire = (
                    len(best_definition_frame) + 4 if best_definitions else 0
                )
                if best_definitions:
                    received_raw = decompressor.decompress(best_definition_frame)
                    if received_raw != definition_raw:
                        raise ValueError("online definition frame mismatch")
                    received_keys = deserialize_definition_batch(
                        received_raw,
                        decoder_regions,
                    )
                    append_package_keys(installed_decoder, received_keys)
                    if installed_decoder.keys != installed_encoder.keys:
                        raise ValueError(
                            "online receiver package differs from encoder package"
                        )
                    result.online_published_assets += len(best_definitions)
                    if seed_only:
                        result.seed_published_assets += sum(
                            key in initial.ids for key in best_definitions
                        )
        else:
            keys = materialization_keys(tu, frozen_baseline)
            payload = encoder.frame(keys)
            payload_frame = compressor.compress(payload)
        result.encode_seconds += time.monotonic() - begin

        begin = time.monotonic()
        receiver_context_raw = (
            decompressor.decompress(context_frame) if canonical_context else b""
        )
        recovered_payload = decompressor.decompress(payload_frame)
        if selected_representation == "atom":
            recovered = decode_canonical_atom_frame(
                recovered_payload,
                installed_decoder,
                decoder_regions,
            )
        elif selected_representation == "prior-root":
            recovered, selected_root_received = deserialize_prior_root(
                recovered_payload,
                prior_root_decoder,
            )
            if (
                recovered != root_copy_recovered
                or selected_root_received != root_copy_received
            ):
                raise ValueError("selected prior-root replay differs from candidate replay")
        else:
            recovered = decoder.frame(recovered_payload)
        receiver_context_lengths = (
            deserialize_context_runs(receiver_context_raw, len(recovered))
            if canonical_context
            else []
        )
        result.decode_seconds += time.monotonic() - begin
        exact = recovered_payload == payload and recovered == expected
        result.exact &= exact
        if not exact:
            raise ValueError(f"{name}: exact replay failed at TU {tu.tu}")

        if canonical_context:
            if receiver_context_lengths != sender_context_lengths:
                raise ValueError("context-run sidecar differs at receiver")
            sender_keys = materialization_keys(tu, installed_encoder)
            receiver_context_tu = tu_with_context_runs(
                recovered,
                receiver_context_lengths,
            )
            receiver_keys = materialization_keys(
                receiver_context_tu,
                installed_decoder,
            )
            if receiver_keys != sender_keys:
                raise ValueError("canonical context parses differ")
            sender_dynamic = advance_canonical_dynamic(
                context_encoder_before,
                sender_keys,
                installed_encoder,
            )
            receiver_dynamic = advance_canonical_dynamic(
                context_decoder_before,
                receiver_keys,
                installed_decoder,
            )
            if receiver_dynamic != sender_dynamic:
                raise ValueError("canonical context dictionaries diverged after TU")
            encoder.dynamic = sender_dynamic
            decoder.dynamic = dense_decoder_dynamic(receiver_dynamic)

        if prior_root_copy:
            prior_root_encoder.commit(expected, root_copy_new_regions)
            prior_root_decoder.commit(root_copy_recovered, root_copy_received)
            prior_root_index.add(expected)
            if (
                prior_root_encoder.ids != prior_root_decoder.ids
                or prior_root_encoder.values != prior_root_decoder.values
                or prior_root_encoder.roots != prior_root_decoder.roots
            ):
                raise ValueError("prior-root encoder and decoder states diverged")

        encoder_regions.observe(expected)
        decoder_regions.observe(recovered)
        if decoder_regions.values != encoder_regions.values:
            raise ValueError("dense Region ID stores diverged")
        if prior_root_copy and prior_root_encoder.ids != encoder_regions.ids:
            raise ValueError("prior-root and dense Region IDs diverged after TU")

        payload_wire = len(payload_frame) + 4
        result.payload_wire_bytes += payload_wire
        result.context_wire_bytes += context_wire
        result.selector_wire_bytes += selector_wire
        result.online_selected_tus += int(used_online)
        result.root_copy_selected_tus += int(root_copy_selected)
        result.root_copy_copies += root_copy_copies if root_copy_selected else 0
        result.root_copy_copied_regions += (
            root_copy_copied_regions if root_copy_selected else 0
        )
        result.raw_bytes += tu.raw_bytes
        result.tus += 1

        begin = time.monotonic()
        promotions = learner.observe(tu)
        result.update_seconds += time.monotonic() - begin

        # The comparison policy makes an investment after observing this TU.
        # These definitions cannot affect the TU that caused their promotion.
        if publication == "promotion" and promotions:
            definition_raw = (
                serialize_mixed_definition_batch(promotions, encoder_regions)
                if seed_only
                else serialize_reference_batch(promotions, encoder_regions)
            )
            definition_frame = compressor.compress(definition_raw)
            append_package_keys(installed_encoder, promotions)
            received_raw = decompressor.decompress(definition_frame)
            if received_raw != definition_raw:
                raise ValueError("promoted definition frame mismatch")
            received_keys = deserialize_definition_batch(
                received_raw,
                decoder_regions,
            )
            append_package_keys(installed_decoder, received_keys)
            if installed_decoder.keys != installed_encoder.keys:
                raise ValueError("promoted receiver package differs from encoder package")
            promoted_wire = len(definition_frame) + 4
            definition_wire += promoted_wire
            result.online_published_assets += len(promotions)
            if seed_only:
                result.seed_published_assets += sum(
                    key in initial.ids for key in promotions
                )

        result.definition_wire_bytes += definition_wire
        result.definition_uncompressed_bytes += len(definition_raw) if definition_wire else 0

        cumulative_wire += (
            definition_wire + context_wire + payload_wire + selector_wire
        )
        if cross_probe_live and ordinal == probe_limit:
            if shadow_probe_wire is None:
                raise ValueError("missing live cross-probe shadow result")
            result.cross_probe_savings = shadow_probe_wire - cumulative_wire
            result.cross_enabled = (
                result.cross_probe_savings >= cross_probe_min_savings
            )
        result.per_tu_curve.append({
            "tu": ordinal,
            "raw_bytes": tu.raw_bytes,
            "payload_wire_bytes": payload_wire,
            "definition_wire_bytes": definition_wire,
            "context_wire_bytes": context_wire,
            "selector_wire_bytes": selector_wire,
            "wire_bytes": (
                payload_wire + definition_wire + context_wire + selector_wire
            ),
            "cumulative_raw_bytes": result.raw_bytes,
            "cumulative_charged_bytes": cumulative_wire,
            "cumulative_charged_ratio": result.raw_bytes / max(1, cumulative_wire),
            "vocabulary_assets": len(learner.vocabulary.keys),
            "online_promoted_assets": learner.promoted_assets,
            "online_published_assets": result.online_published_assets,
            "seed_published_assets": result.seed_published_assets,
            "online_selected_tus": result.online_selected_tus,
            "root_copy_selected": root_copy_selected,
            "root_copy_copies": root_copy_copies if root_copy_selected else 0,
            "root_copy_copied_regions": (
                root_copy_copied_regions if root_copy_selected else 0
            ),
            "root_copy_receiver_state_bytes": (
                prior_root_index.root_vector_logical_bytes
            ),
            "root_copy_encoder_state_bytes": prior_root_index.encoder_logical_bytes,
            "candidate_state_bytes": learner.counts.logical_bytes,
            "logical_state_bytes": learner.logical_state_bytes,
        })

    result.online_definition_raw_bytes = learner.promoted_raw_bytes
    result.online_promoted_assets = learner.promoted_assets
    result.candidate_observations = learner.candidate_observations
    result.final_logical_state_bytes = learner.logical_state_bytes
    result.root_copy_receiver_state_bytes = prior_root_index.root_vector_logical_bytes
    result.root_copy_encoder_state_bytes = prior_root_index.encoder_logical_bytes
    return result


def write_curve(rows: Sequence[dict], path: str) -> None:
    columns = (
        "row",
        "tu",
        "raw_bytes",
        "payload_wire_bytes",
        "definition_wire_bytes",
        "context_wire_bytes",
        "selector_wire_bytes",
        "wire_bytes",
        "cumulative_raw_bytes",
        "cumulative_charged_bytes",
        "cumulative_charged_ratio",
        "vocabulary_assets",
        "online_promoted_assets",
        "online_published_assets",
        "seed_published_assets",
        "online_selected_tus",
        "root_copy_selected",
        "root_copy_copies",
        "root_copy_copied_regions",
        "root_copy_receiver_state_bytes",
        "root_copy_encoder_state_bytes",
        "candidate_state_bytes",
        "logical_state_bytes",
    )
    with open(path, "w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns, delimiter="\t")
        writer.writeheader()
        for row in rows:
            for point in row["per_tu_curve"]:
                writer.writerow({"row": row["name"], **point})


def load_package(path: str) -> tuple[StaticPackage, bytes]:
    frame = Path(path).read_bytes()
    raw = decompress_frame(frame)
    keys = deserialize_key_batch(raw)
    if serialize_key_batch(keys) != raw:
        raise ValueError("loaded package is not canonical")
    return package_from_keys(keys, len(raw)), frame


def load_target(
    path: str,
    max_tus: int,
    order: str,
    order_seed: int,
    perturb_shared_region: bool,
) -> tuple[list[TuRegions], dict]:
    target = list(iter_tus_fast(path))
    if max_tus:
        target = target[:max_tus]
    details: dict = {
        "order": order,
        "order_seed": order_seed,
        "perturb_shared_region": perturb_shared_region,
        "tus": len(target),
        "raw_bytes": sum(tu.raw_bytes for tu in target),
    }

    if perturb_shared_region:
        support: collections.Counter[tuple[int, int, int]] = collections.Counter()
        all_regions: set[tuple[int, int, int]] = set()
        for tu in target:
            current = {(h1, h2, raw_len) for h1, h2, _, raw_len in tu.regions}
            support.update(current)
            all_regions.update(current)
        if not support:
            raise ValueError("cannot perturb an empty Region stream")
        original, tu_support = min(support.items(), key=lambda item: (-item[1], item[0]))
        h1, h2, raw_len = original
        replacement = (
            h1 ^ 0x9E3779B97F4A7C15,
            h2 ^ 0xD1B54A32D192ED03,
            raw_len,
        )
        while replacement in all_regions:
            replacement = (
                (replacement[0] + 0x9E3779B97F4A7C15) & ((1 << 64) - 1),
                (replacement[1] + 0xD1B54A32D192ED03) & ((1 << 64) - 1),
                raw_len,
            )
        affected_tus = affected_occurrences = 0
        changed: list[TuRegions] = []
        for tu in target:
            regions: list[tuple[int, int, int, int]] = []
            affected = False
            for old_h1, old_h2, context, old_raw_len in tu.regions:
                if (old_h1, old_h2, old_raw_len) == original:
                    regions.append((replacement[0], replacement[1], context, old_raw_len))
                    affected = True
                    affected_occurrences += 1
                else:
                    regions.append((old_h1, old_h2, context, old_raw_len))
            affected_tus += int(affected)
            changed.append(TuRegions(tu.tu, tu.raw_bytes, regions))
        target = changed
        details["perturbation"] = {
            "original": list(original),
            "replacement": list(replacement),
            "tu_support": tu_support,
            "affected_tus": affected_tus,
            "affected_occurrences": affected_occurrences,
        }

    if order == "shuffled":
        random.Random(order_seed).shuffle(target)
    elif order == "reverse":
        target.reverse()
    return target, details


def run(args: argparse.Namespace) -> int:
    started = time.monotonic()
    if args.package_in:
        initial, input_frame = load_package(args.package_in)
        training: dict = {"loaded_package": args.package_in}
    else:
        candidates, training = collect_candidates(
            args.train,
            args.training_candidates,
            args.phrase_lengths,
        )
        initial = build_package(candidates, args.static_budget)
        input_frame = b""

    initial_raw = serialize_key_batch(initial.keys)
    canonical_frame = compress_frame(initial_raw, args.model_level)
    if args.package_out:
        Path(args.package_out).write_bytes(canonical_frame)
        saved_frame = canonical_frame
    elif input_frame:
        saved_frame = input_frame
    else:
        saved_frame = canonical_frame
    if (
        decompress_frame(saved_frame) != initial_raw
        or serialize_key_batch(deserialize_key_batch(initial_raw)) != initial_raw
    ):
        raise ValueError("package changed after canonical reconstruction")

    # Freeze the complete model artifact before the target trace is opened.
    target_tus, target_details = load_target(
        args.test,
        args.max_tus,
        args.order,
        args.order_seed,
        args.perturb_shared_region,
    )

    empty = package_from_keys([], 0)
    specifications: list[tuple[str, StaticPackage, int, int, bool]] = []
    if args.row_set in ("all", "empty"):
        specifications.append(("empty-frozen", empty, 0, 1, False))
    if (
        args.row_set in ("all", "pretrained")
        and args.pretrained_mode == "installed"
    ):
        specifications.append(("pretrained-frozen", initial, 0, 1, False))
    for threshold in args.thresholds:
        if args.row_set in ("all", "empty", "online"):
            specifications.append(
                (
                    f"empty-online-k{threshold}",
                    empty,
                    args.online_budget,
                    threshold,
                    False,
                )
            )
        if args.row_set in ("all", "pretrained", "online"):
            seed_only = args.pretrained_mode == "seed-only"
            specifications.append(
                (
                    f"pretrained-{'seed-' if seed_only else ''}online-k{threshold}",
                    initial,
                    args.online_budget,
                    threshold,
                    seed_only,
                )
            )

    rows: list[dict] = []
    for name, package, online_budget, threshold, seed_only in specifications:
        row = evaluate_online(
            target_tus,
            name,
            package,
            saved_frame if package.keys else compress_frame(serialize_key_batch([]), args.model_level),
            args.level,
            args.phrase_lengths,
            args.phrase_scope,
            args.parse_policy,
            args.cross_probe_tus,
            args.cross_probe_min_savings,
            args.cross_probe_live,
            threshold,
            online_budget,
            args.per_tu_budget,
            args.candidate_capacity,
            args.maximum_key_bytes,
            args.publication,
            args.budget_basis,
            seed_only,
            prior_root_copy=args.prior_root_copy and bool(online_budget),
            root_copy_seed_length=args.root_copy_seed_length,
            root_copy_candidates=args.root_copy_candidates,
            root_copy_index_stride=args.root_copy_index_stride,
        ).as_dict()
        rows.append(row)
        print(json.dumps({
            "name": name,
            "tus": row["tus"],
            "charged_wire_bytes": row["charged_wire_bytes"],
            "charged_ratio": row["charged_ratio"],
            "initial_model_wire_bytes": row["initial_model_wire_bytes"],
            "pretrained_mode": row["pretrained_mode"],
            "seed_model_compressed_bytes": row["seed_model_compressed_bytes"],
            "seed_published_assets": row["seed_published_assets"],
            "online_definition_raw_bytes": row["online_definition_raw_bytes"],
            "definition_wire_bytes": row["definition_wire_bytes"],
            "context_wire_bytes": row["context_wire_bytes"],
            "selector_wire_bytes": row["selector_wire_bytes"],
            "promoted": row["online_promoted_assets"],
            "published": row["online_published_assets"],
            "selected_tus": row["online_selected_tus"],
            "root_copy_selected_tus": row["root_copy_selected_tus"],
            "root_copy_copies": row["root_copy_copies"],
            "root_copy_copied_regions": row["root_copy_copied_regions"],
            "root_copy_receiver_state_bytes": row[
                "root_copy_receiver_state_bytes"
            ],
            "root_copy_encoder_state_bytes": row[
                "root_copy_encoder_state_bytes"
            ],
            "state_bytes": row["final_logical_state_bytes"],
            "exact": row["exact"],
        }), flush=True)

    report = {
        "experiment": "causal_exact_phrase_empty_vs_pretrained_bootstrap",
        "test": args.test,
        "target": target_details,
        "training": training,
        "level": args.level,
        "model_level": args.model_level,
        "phrase_lengths": args.phrase_lengths,
        "phrase_scope": args.phrase_scope,
        "parse_policy": args.parse_policy,
        "cross_probe_tus": args.cross_probe_tus,
        "cross_probe_min_savings": args.cross_probe_min_savings,
        "cross_probe_live": args.cross_probe_live,
        "prior_root_copy": args.prior_root_copy,
        "root_copy_seed_length": args.root_copy_seed_length,
        "root_copy_candidates": args.root_copy_candidates,
        "root_copy_index_stride": args.root_copy_index_stride,
        "static_budget": args.static_budget,
        "online_budget": args.online_budget,
        "per_tu_budget": args.per_tu_budget,
        "candidate_capacity": args.candidate_capacity,
        "training_candidates": args.training_candidates,
        "maximum_key_bytes": args.maximum_key_bytes,
        "publication": args.publication,
        "budget_basis": args.budget_basis,
        "pretrained_mode": args.pretrained_mode,
        "row_set": args.row_set,
        "max_tus": args.max_tus,
        "initial_package": {
            "assets": len(initial.keys),
            "raw_bytes": len(initial_raw),
            "wire_bytes": len(saved_frame) + 4,
            "sha256": hashlib.sha256(saved_frame).hexdigest(),
            "path": args.package_out or args.package_in,
        },
        "rows": rows,
        "wall_seconds": time.monotonic() - started,
    }
    Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.curve_tsv:
        write_curve(rows, args.curve_tsv)
    print(json.dumps({
        "report": args.report,
        "curve": args.curve_tsv,
        "package_sha256": report["initial_package"]["sha256"],
        "wall_seconds": report["wall_seconds"],
    }))
    return 0


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--test", required=True)
    source = value.add_mutually_exclusive_group(required=True)
    source.add_argument("--train", nargs="+")
    source.add_argument("--package-in")
    value.add_argument("--package-out")
    value.add_argument("--static-budget", type=int, default=256 << 10)
    value.add_argument("--online-budget", type=int, default=256 << 10)
    value.add_argument("--per-tu-budget", type=int, default=8 << 10)
    value.add_argument("--candidate-capacity", type=int, default=250_000)
    value.add_argument("--training-candidates", type=int, default=750_000)
    value.add_argument("--maximum-key-bytes", type=int, default=8 << 10)
    value.add_argument("--phrase-lengths", nargs="+", type=int, default=(2, 4, 8, 16, 32))
    value.add_argument(
        "--phrase-scope",
        choices=(
            "run",
            "run-and-tu",
            "run-vs-tu-match",
            "run-vs-tu-match-canonical",
            "run-vs-tu-match-context",
            "run-vs-tu-match-hybrid",
            "run-plus-static-cross",
        ),
        default="run",
    )
    value.add_argument(
        "--parse-policy",
        choices=("greedy", "greedy-plus-coverage"),
        default="greedy",
    )
    value.add_argument("--cross-probe-tus", type=int, default=0)
    value.add_argument("--cross-probe-min-savings", type=int, default=0)
    value.add_argument("--cross-probe-live", action="store_true")
    value.add_argument("--prior-root-copy", action="store_true")
    value.add_argument("--root-copy-seed-length", type=int, default=4)
    value.add_argument("--root-copy-candidates", type=int, default=4)
    value.add_argument("--root-copy-index-stride", type=int, default=1)
    value.add_argument("--thresholds", nargs="+", type=int, default=(2, 3, 4, 6))
    value.add_argument("--level", type=int, choices=(1, 3), default=1)
    value.add_argument("--model-level", type=int, choices=(1, 3, 6), default=3)
    value.add_argument("--max-tus", type=int, default=0)
    value.add_argument(
        "--order",
        choices=("standard", "shuffled", "reverse"),
        default="standard",
    )
    value.add_argument("--order-seed", type=int, default=0x51B10C)
    value.add_argument("--perturb-shared-region", action="store_true")
    value.add_argument(
        "--publication",
        choices=("promotion", "first-use"),
        default="first-use",
    )
    value.add_argument("--budget-basis", choices=("raw", "ids32"), default="ids32")
    value.add_argument(
        "--pretrained-mode",
        choices=("installed", "seed-only"),
        default="installed",
    )
    value.add_argument(
        "--row-set",
        choices=("all", "empty", "pretrained", "online"),
        default="all",
        help="online selects empty-online and pretrained-online without frozen rows",
    )
    value.add_argument("--curve-tsv")
    value.add_argument("--report", required=True)
    return value


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
