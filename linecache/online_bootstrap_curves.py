#!/usr/bin/env python3
"""Causal exact-phrase bootstrap curves for issue #16.

This is a focused capability harness.  It compares the *same* exact Region-phrase
learner in two initial states:

* empty vocabulary at target TU 0;
* a frozen exact phrase package learned from disjoint projects and charged at TU 0.

Target-local phrases are observed only after their TU has been encoded.  The primary
policy publishes a promoted phrase after that TU as a causal investment; an optional
first-use policy defers publication until a later TU selects it.  The receiver
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
from typing import Iterable, Sequence

import zstandard as zstd

from pretrained_superblocks import (
    ExactDecoder,
    ExactEncoder,
    StaticPackage,
    TuRegions,
    build_package,
    collect_candidates,
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


MODEL_VERSION = 1
REFERENCE_MODEL_VERSION = 1


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


def clone_encoder(source: ExactEncoder, package: StaticPackage) -> ExactEncoder:
    clone = ExactEncoder(package, {})
    clone.dynamic = dict(source.dynamic)
    return clone


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
    ):
        self.vocabulary = clone_package(initial)
        self.initial_assets = len(initial.keys)
        self.phrase_lengths = tuple(phrase_lengths)
        self.threshold = threshold
        self.online_budget = online_budget
        self.per_tu_budget = per_tu_budget
        self.maximum_key_bytes = maximum_key_bytes
        self.budget_basis = budget_basis
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
        for key in phrase_candidates(tu, self.phrase_lengths):
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
    raw_bytes: int = 0
    payload_wire_bytes: int = 0
    definition_wire_bytes: int = 0
    definition_uncompressed_bytes: int = 0
    selector_wire_bytes: int = 0
    online_definition_raw_bytes: int = 0
    online_promoted_assets: int = 0
    online_published_assets: int = 0
    online_selected_tus: int = 0
    candidate_observations: int = 0
    final_logical_state_bytes: int = 0
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
    threshold: int,
    online_budget: int,
    per_tu_budget: int,
    candidate_capacity: int,
    maximum_key_bytes: int,
    publication: str,
    budget_basis: str,
) -> OnlineResult:
    initial_raw = decompress_frame(initial_frame)
    if deserialize_key_batch(initial_raw) != initial.keys:
        raise ValueError("supplied initial model frame differs from encoder package")
    installed_encoder = clone_package(initial)
    installed_decoder = package_from_keys([], 0)
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
    learner = CausalPhraseLearner(
        initial,
        phrase_lengths,
        threshold,
        online_budget,
        per_tu_budget,
        candidate_capacity,
        maximum_key_bytes,
        budget_basis,
    )
    result = OnlineResult(
        name=name,
        initial_assets=len(initial.keys),
        initial_model_raw_bytes=len(initial_raw),
        initial_model_wire_bytes=(len(initial_frame) + 4 if initial.keys else 0),
        online_budget=online_budget,
        promotion_threshold=threshold,
        per_tu_budget=per_tu_budget,
        candidate_capacity=candidate_capacity,
        publication=publication,
        budget_basis=budget_basis,
    )
    compressor = zstd.ZstdCompressor(level=level)
    decompressor = zstd.ZstdDecompressor()
    cumulative_wire = result.initial_model_wire_bytes

    for ordinal, tu in enumerate(target_tus, 1):
        begin = time.monotonic()
        selector_wire = 1 if online_budget else 0
        definition_wire = 0
        used_online = False

        if online_budget:
            # Compare both representations from exactly the same prior dynamic
            # dictionary.  The baseline retains the frozen initial vocabulary;
            # the online candidate may use all causally promoted phrases.
            baseline_keys = materialization_keys(tu, initial)
            baseline_encoder = clone_encoder(encoder, installed_encoder)
            baseline_payload = baseline_encoder.frame(baseline_keys)
            baseline_frame = compressor.compress(baseline_payload)

            online_keys = materialization_keys(tu, learner.vocabulary)
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
            online_payload = online_encoder.frame(online_keys)
            online_frame = compressor.compress(online_payload)
            definition_raw = serialize_reference_batch(new_definitions, encoder_regions)
            definition_frame = (
                compressor.compress(definition_raw) if new_definitions else b""
            )
            candidate_definition_wire = (
                len(definition_frame) + 4 if new_definitions else 0
            )

            baseline_wire = len(baseline_frame) + 4 + selector_wire
            online_wire = (
                len(online_frame)
                + 4
                + candidate_definition_wire
                + selector_wire
            )
            if online_wire < baseline_wire:
                used_online = True
                installed_encoder = candidate_package
                encoder = online_encoder
                payload = online_payload
                payload_frame = online_frame
                definition_wire = candidate_definition_wire
                if new_definitions:
                    received_raw = decompressor.decompress(definition_frame)
                    if received_raw != definition_raw:
                        raise ValueError("online definition frame mismatch")
                    received_keys = deserialize_reference_batch(received_raw, decoder_regions)
                    append_package_keys(installed_decoder, received_keys)
                    if installed_decoder.keys != installed_encoder.keys:
                        raise ValueError(
                            "online receiver package differs from encoder package"
                        )
                    result.online_published_assets += len(new_definitions)
            else:
                encoder = baseline_encoder
                payload = baseline_payload
                payload_frame = baseline_frame
        else:
            keys = materialization_keys(tu, initial)
            payload = encoder.frame(keys)
            payload_frame = compressor.compress(payload)
        result.encode_seconds += time.monotonic() - begin

        begin = time.monotonic()
        recovered_payload = decompressor.decompress(payload_frame)
        recovered = decoder.frame(recovered_payload)
        result.decode_seconds += time.monotonic() - begin
        exact = recovered_payload == payload and recovered == expected_regions(tu)
        result.exact &= exact
        if not exact:
            raise ValueError(f"{name}: exact replay failed at TU {tu.tu}")

        encoder_regions.observe(expected_regions(tu))
        decoder_regions.observe(recovered)
        if decoder_regions.values != encoder_regions.values:
            raise ValueError("dense Region ID stores diverged")

        payload_wire = len(payload_frame) + 4
        result.payload_wire_bytes += payload_wire
        result.selector_wire_bytes += selector_wire
        result.online_selected_tus += int(used_online)
        result.raw_bytes += tu.raw_bytes
        result.tus += 1

        begin = time.monotonic()
        promotions = learner.observe(tu)
        result.update_seconds += time.monotonic() - begin

        # The primary policy makes a causal investment after observing this TU.
        # These definitions cannot affect the TU that caused their promotion.
        if publication == "promotion" and promotions:
            definition_raw = serialize_reference_batch(promotions, encoder_regions)
            definition_frame = compressor.compress(definition_raw)
            append_package_keys(installed_encoder, promotions)
            received_raw = decompressor.decompress(definition_frame)
            if received_raw != definition_raw:
                raise ValueError("promoted definition frame mismatch")
            received_keys = deserialize_reference_batch(received_raw, decoder_regions)
            append_package_keys(installed_decoder, received_keys)
            if installed_decoder.keys != installed_encoder.keys:
                raise ValueError("promoted receiver package differs from encoder package")
            promoted_wire = len(definition_frame) + 4
            definition_wire += promoted_wire
            result.online_published_assets += len(promotions)

        result.definition_wire_bytes += definition_wire
        result.definition_uncompressed_bytes += len(definition_raw) if definition_wire else 0

        cumulative_wire += definition_wire + payload_wire + selector_wire
        result.per_tu_curve.append({
            "tu": ordinal,
            "raw_bytes": tu.raw_bytes,
            "payload_wire_bytes": payload_wire,
            "definition_wire_bytes": definition_wire,
            "selector_wire_bytes": selector_wire,
            "wire_bytes": payload_wire + definition_wire + selector_wire,
            "cumulative_raw_bytes": result.raw_bytes,
            "cumulative_charged_bytes": cumulative_wire,
            "cumulative_charged_ratio": result.raw_bytes / max(1, cumulative_wire),
            "vocabulary_assets": len(learner.vocabulary.keys),
            "online_promoted_assets": learner.promoted_assets,
            "online_published_assets": result.online_published_assets,
            "online_selected_tus": result.online_selected_tus,
            "candidate_state_bytes": learner.counts.logical_bytes,
            "logical_state_bytes": learner.logical_state_bytes,
        })

    result.online_definition_raw_bytes = learner.promoted_raw_bytes
    result.online_promoted_assets = learner.promoted_assets
    result.candidate_observations = learner.candidate_observations
    result.final_logical_state_bytes = learner.logical_state_bytes
    return result


def write_curve(rows: Sequence[dict], path: str) -> None:
    columns = (
        "row",
        "tu",
        "raw_bytes",
        "payload_wire_bytes",
        "definition_wire_bytes",
        "selector_wire_bytes",
        "wire_bytes",
        "cumulative_raw_bytes",
        "cumulative_charged_bytes",
        "cumulative_charged_ratio",
        "vocabulary_assets",
        "online_promoted_assets",
        "online_published_assets",
        "online_selected_tus",
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
    specifications: list[tuple[str, StaticPackage, int, int]] = []
    if args.row_set in ("all", "empty"):
        specifications.append(("empty-frozen", empty, 0, 1))
    if args.row_set in ("all", "pretrained"):
        specifications.append(("pretrained-frozen", initial, 0, 1))
    for threshold in args.thresholds:
        if args.row_set in ("all", "empty"):
            specifications.append(
                (f"empty-online-k{threshold}", empty, args.online_budget, threshold)
            )
        if args.row_set in ("all", "pretrained"):
            specifications.append(
                (f"pretrained-online-k{threshold}", initial, args.online_budget, threshold)
            )

    rows: list[dict] = []
    for name, package, online_budget, threshold in specifications:
        row = evaluate_online(
            target_tus,
            name,
            package,
            saved_frame if package.keys else compress_frame(serialize_key_batch([]), args.model_level),
            args.level,
            args.phrase_lengths,
            threshold,
            online_budget,
            args.per_tu_budget,
            args.candidate_capacity,
            args.maximum_key_bytes,
            args.publication,
            args.budget_basis,
        ).as_dict()
        rows.append(row)
        print(json.dumps({
            "name": name,
            "tus": row["tus"],
            "charged_wire_bytes": row["charged_wire_bytes"],
            "charged_ratio": row["charged_ratio"],
            "initial_model_wire_bytes": row["initial_model_wire_bytes"],
            "online_definition_raw_bytes": row["online_definition_raw_bytes"],
            "definition_wire_bytes": row["definition_wire_bytes"],
            "selector_wire_bytes": row["selector_wire_bytes"],
            "promoted": row["online_promoted_assets"],
            "published": row["online_published_assets"],
            "selected_tus": row["online_selected_tus"],
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
        "static_budget": args.static_budget,
        "online_budget": args.online_budget,
        "per_tu_budget": args.per_tu_budget,
        "candidate_capacity": args.candidate_capacity,
        "training_candidates": args.training_candidates,
        "maximum_key_bytes": args.maximum_key_bytes,
        "publication": args.publication,
        "budget_basis": args.budget_basis,
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
    value.add_argument("--thresholds", nargs="+", type=int, default=(2, 3, 4, 6))
    value.add_argument("--level", type=int, choices=(1, 3), default=1)
    value.add_argument("--model-level", type=int, choices=(1, 3, 6), default=3)
    value.add_argument("--max-tus", type=int, default=0)
    value.add_argument("--order", choices=("standard", "shuffled"), default="standard")
    value.add_argument("--order-seed", type=int, default=0x51B10C)
    value.add_argument("--perturb-shared-region", action="store_true")
    value.add_argument(
        "--publication",
        choices=("promotion", "first-use"),
        default="promotion",
    )
    value.add_argument("--budget-basis", choices=("raw", "ids32"), default="ids32")
    value.add_argument("--row-set", choices=("all", "empty", "pretrained"), default="all")
    value.add_argument("--curve-tsv")
    value.add_argument("--report", required=True)
    return value


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
