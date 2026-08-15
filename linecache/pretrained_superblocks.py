#!/usr/bin/env python3
"""Disjoint-project pretraining experiments for issue #16 superblock streams.

This is a capability harness, not the production hot path.  It measures three separable ideas:

* immutable exact superblocks selected from unrelated preprocessed projects;
* a causal next-superblock model, distilled to a fixed context -> top-K table;
* tiny GRU and TCN teachers that measure whether the fixed table leaves useful predictability.

Every reported codec row emits real framed bytes, decodes with an independent state object, and
compares the reconstructed Region-digest stream byte-for-byte.  Training projects are read before
the held-out project.  No target event changes the pretrained package or predictor table.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import hashlib
import json
import math
import os
import random
import struct
import time
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, Sequence

import numpy as np
import torch
import zstandard as zstd
from torch import nn
from torch.utils.data import DataLoader, TensorDataset

from ml_models import CANDIDATE, EVENT_FIXED, FOOTER, REGION, TU_HEAD, U32, read_exact


MAGIC = b"ICMLDS2\0"
MODEL_VERSION = 1
REGION_TRIPLE = struct.Struct("<QQI")


@dataclasses.dataclass(slots=True)
class TuRegions:
    tu: int
    raw_bytes: int
    regions: list[tuple[int, int, int, int]]


def iter_tus_fast(path: str | os.PathLike[str]) -> Iterator[TuRegions]:
    """Read only TU/Region records and seek over the large Line-candidate payload."""
    with open(path, "rb") as f:
        if read_exact(f, len(MAGIC)) != MAGIC:
            raise ValueError(f"{path}: wrong dataset magic")
        version = U32.unpack(read_exact(f, U32.size))[0]
        if version != 2:
            raise ValueError(f"{path}: unsupported schema {version}")
        while True:
            tag_raw = f.read(1)
            if len(tag_raw) != 1:
                raise EOFError(f"{path}: missing footer")
            tag = tag_raw[0]
            if tag == 0:
                read_exact(f, FOOTER.size)
                if f.read(1):
                    raise ValueError(f"{path}: trailing bytes")
                return
            if tag == 1:
                tu, raw_bytes, count = TU_HEAD.unpack(read_exact(f, TU_HEAD.size))
                regions = [REGION.unpack(read_exact(f, REGION.size)) for _ in range(count)]
                yield TuRegions(tu, raw_bytes, regions)
                continue
            if tag != 2:
                raise ValueError(f"{path}: unknown record tag {tag}")
            read_exact(f, EVENT_FIXED.size)
            line_len = U32.unpack(read_exact(f, U32.size))[0]
            f.seek(line_len, os.SEEK_CUR)
            candidate_count = U32.unpack(read_exact(f, U32.size))[0]
            f.seek(candidate_count * CANDIDATE.size, os.SEEK_CUR)


def put_varint(value: int) -> bytes:
    if value < 0:
        raise ValueError(value)
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def get_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while offset < len(data) and shift <= 63:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
    raise ValueError("truncated/overlong varint")


def superblock_keys(tu: TuRegions) -> list[bytes]:
    """Use the current semantic-context runs as exact immutable superblock candidates."""
    out: list[bytes] = []
    i = 0
    while i < len(tu.regions):
        context = tu.regions[i][2]
        j = i + 1
        while j < len(tu.regions) and tu.regions[j][2] == context:
            j += 1
        value = bytearray(put_varint(j - i))
        for hash1, hash2, _, raw_len in tu.regions[i:j]:
            value += REGION_TRIPLE.pack(hash1, hash2, raw_len)
        out.append(bytes(value))
        i = j
    return out


def encode_regions(regions: Sequence[tuple[int, int, int]]) -> bytes:
    value = bytearray(put_varint(len(regions)))
    for hash1, hash2, raw_len in regions:
        value += REGION_TRIPLE.pack(hash1, hash2, raw_len)
    return bytes(value)


def context_runs(tu: TuRegions) -> list[list[tuple[int, int, int]]]:
    out: list[list[tuple[int, int, int]]] = []
    i = 0
    while i < len(tu.regions):
        context = tu.regions[i][2]
        j = i + 1
        while j < len(tu.regions) and tu.regions[j][2] == context:
            j += 1
        out.append([(h1, h2, raw_len) for h1, h2, _, raw_len in tu.regions[i:j]])
        i = j
    return out


def phrase_candidates(tu: TuRegions, lengths: Sequence[int]) -> Iterator[bytes]:
    """Stable-boundary overlapping phrases; overlap makes insertions perturb only nearby assets."""
    for run in context_runs(tu):
        yield encode_regions(run)
        for length in lengths:
            if length > len(run):
                continue
            stride = max(1, length // 2)
            starts = list(range(0, len(run) - length + 1, stride))
            final = len(run) - length
            if not starts or starts[-1] != final:
                starts.append(final)
            for start in starts:
                yield encode_regions(run[start:start + length])


def decode_key(key: bytes) -> list[tuple[int, int, int]]:
    count, offset = get_varint(key, 0)
    needed = offset + count * REGION_TRIPLE.size
    if needed != len(key):
        raise ValueError("bad exact superblock definition")
    return [REGION_TRIPLE.unpack_from(key, offset + i * REGION_TRIPLE.size)
            for i in range(count)]


def expected_regions(tu: TuRegions) -> list[tuple[int, int, int]]:
    return [(h1, h2, raw_len) for h1, h2, _, raw_len in tu.regions]


class SpaceSaving:
    """Bound memory while retaining the high-frequency exact superblock candidates."""

    def __init__(self, capacity: int):
        self.capacity = capacity
        self.counts: dict[bytes, int] = {}
        self.error: dict[bytes, int] = {}
        self.heap: list[tuple[int, int, bytes]] = []
        self.serial = 0

    def add(self, key: bytes) -> None:
        import heapq
        self.serial += 1
        if key in self.counts:
            self.counts[key] += 1
            heapq.heappush(self.heap, (self.counts[key], self.serial, key))
            if len(self.heap) > max(10_000, 3 * self.capacity):
                self.heap = [(count, i, value)
                             for i, (value, count) in enumerate(self.counts.items())]
                heapq.heapify(self.heap)
            return
        if len(self.counts) < self.capacity:
            self.counts[key] = 1
            self.error[key] = 0
            heapq.heappush(self.heap, (1, self.serial, key))
            return
        while self.heap:
            count, _, victim = heapq.heappop(self.heap)
            if self.counts.get(victim) == count:
                break
        else:
            raise RuntimeError("empty SpaceSaving heap")
        del self.counts[victim]
        del self.error[victim]
        self.counts[key] = count + 1
        self.error[key] = count
        heapq.heappush(self.heap, (count + 1, self.serial, key))

    def rows(self) -> list[tuple[bytes, int, int]]:
        return [(key, count, self.error[key]) for key, count in self.counts.items()]


def collect_candidates(paths: Sequence[str], capacity: int,
                       phrase_lengths: Sequence[int]) -> tuple[list[tuple[bytes, int]], dict]:
    counter = SpaceSaving(capacity)
    tus = blocks = regions = raw = observations = 0
    begin = time.monotonic()
    for path in paths:
        for tu in iter_tus_fast(path):
            keys = superblock_keys(tu)
            for key in phrase_candidates(tu, phrase_lengths):
                counter.add(key)
                observations += 1
            tus += 1
            blocks += len(keys)
            regions += len(tu.regions)
            raw += tu.raw_bytes
    # Lower bounds are used for ranking so replacement error cannot make a late candidate look
    # artificially frequent.  Byte savings per package byte is the static-model MDL objective.
    rows = [(key, max(1, count - error)) for key, count, error in counter.rows()]
    rows.sort(key=lambda row: (-(row[1] * max(1, len(row[0]) - 2) / (len(row[0]) + 4)),
                               -row[1], row[0]))
    return rows, {
        "tus": tus, "blocks": blocks, "regions": regions, "raw_bytes": raw,
        "candidate_observations": observations, "phrase_lengths": list(phrase_lengths),
        "retained_candidates": len(rows), "seconds": time.monotonic() - begin,
    }


@dataclasses.dataclass(slots=True)
class StaticPackage:
    keys: list[bytes]
    ids: dict[bytes, int]
    definition_bytes: int
    by_first: dict[tuple[int, int, int], list[tuple[int, bytes, list[tuple[int, int, int]]]]]


def package_from_keys(keys: list[bytes], definition_bytes: int) -> StaticPackage:
    ids = {key: i + 1 for i, key in enumerate(keys)}
    by_first: dict[tuple[int, int, int],
                   list[tuple[int, bytes, list[tuple[int, int, int]]]]] = collections.defaultdict(list)
    for static_id, key in enumerate(keys, 1):
        decoded = decode_key(key)
        if decoded:
            by_first[decoded[0]].append((static_id, key, decoded))
    for values in by_first.values():
        values.sort(key=lambda value: (-len(value[2]), value[0]))
    return StaticPackage(keys, ids, definition_bytes, dict(by_first))


def build_package(candidates: Sequence[tuple[bytes, int]], budget: int) -> StaticPackage:
    keys: list[bytes] = []
    used = 0
    for key, _ in candidates:
        static_id = len(keys) + 1
        charge = 1 + len(put_varint(static_id)) + len(put_varint(len(key))) + len(key)
        if used + charge > budget:
            continue
        keys.append(key)
        used += charge
    return package_from_keys(keys, used)


def materialization_keys(tu: TuRegions, package: StaticPackage) -> list[bytes]:
    """Greedy longest exact static phrase cover, with dynamic raw gaps as universal fallback."""
    if not package.keys:
        return superblock_keys(tu)
    out: list[bytes] = []
    for run in context_runs(tu):
        raw: list[tuple[int, int, int]] = []
        i = 0
        while i < len(run):
            selected: bytes | None = None
            selected_len = 0
            for _, key, phrase in package.by_first.get(run[i], ()):
                length = len(phrase)
                if i + length <= len(run) and run[i:i + length] == phrase:
                    selected = key
                    selected_len = length
                    break
            if selected is None:
                raw.append(run[i])
                i += 1
                continue
            if raw:
                out.append(encode_regions(raw))
                raw.clear()
            out.append(selected)
            i += selected_len
        if raw:
            out.append(encode_regions(raw))
    return out


Context = tuple[int, int]
PredictionMap = dict[Context, tuple[int, ...]]


def varint_size(value: int) -> int:
    return len(put_varint(value))


def prediction_package_bytes(predictions: PredictionMap) -> int:
    total = len(put_varint(len(predictions)))
    for (a, b), values in predictions.items():
        total += varint_size(a) + varint_size(b) + varint_size(len(values))
        total += sum(varint_size(value) for value in values)
    return total


def learn_ngram_map(paths: Sequence[str], package: StaticPackage, k: int,
                    maximum_contexts: int) -> PredictionMap:
    counts: dict[Context, collections.Counter[int]] = collections.defaultdict(collections.Counter)
    context_uses: collections.Counter[Context] = collections.Counter()
    for path in paths:
        for tu in iter_tus_fast(path):
            a = b = 0
            for key in materialization_keys(tu, package):
                token = package.ids.get(key, 0)
                if token:
                    counts[(a, b)][token] += 1
                    context_uses[(a, b)] += 1
                    a, b = b, token
                else:
                    a = b = 0
    selected = context_uses.most_common(maximum_contexts)
    return {context: tuple(value for value, _ in counts[context].most_common(k))
            for context, _ in selected if counts[context]}


class ExactEncoder:
    RAW_DEFINE = 0
    DYNAMIC_REF = 1
    STATIC_ID = 2
    PREDICTED = 3

    def __init__(self, package: StaticPackage, predictions: PredictionMap):
        self.package = package
        self.predictions = predictions
        self.dynamic: dict[bytes, int] = {}

    def frame(self, keys: Sequence[bytes]) -> bytes:
        out = bytearray(put_varint(len(keys)))
        a = b = 0
        for key in keys:
            static_id = self.package.ids.get(key, 0)
            if static_id:
                predicted = self.predictions.get((a, b), ())
                try:
                    rank = predicted.index(static_id)
                except ValueError:
                    rank = -1
                static_cost = 1 + varint_size(static_id)
                if 0 <= rank < 256 and 2 <= static_cost:
                    out.extend((self.PREDICTED, rank))
                else:
                    out.append(self.STATIC_ID)
                    out += put_varint(static_id)
                a, b = b, static_id
                continue
            dynamic_id = self.dynamic.get(key)
            if dynamic_id is None:
                dynamic_id = len(self.dynamic) + 1
                self.dynamic[key] = dynamic_id
                out.append(self.RAW_DEFINE)
                out += put_varint(dynamic_id)
                out += put_varint(len(key))
                out += key
            else:
                out.append(self.DYNAMIC_REF)
                out += put_varint(dynamic_id)
            a = b = 0
        return bytes(out)


class ExactDecoder:
    def __init__(self, package: StaticPackage, predictions: PredictionMap):
        self.package = package
        self.predictions = predictions
        self.dynamic: list[bytes] = [b""]

    def frame(self, data: bytes) -> list[tuple[int, int, int]]:
        count, offset = get_varint(data, 0)
        keys: list[bytes] = []
        a = b = 0
        for _ in range(count):
            if offset >= len(data):
                raise ValueError("truncated superblock opcode")
            op = data[offset]
            offset += 1
            if op == ExactEncoder.RAW_DEFINE:
                dynamic_id, offset = get_varint(data, offset)
                size, offset = get_varint(data, offset)
                if dynamic_id != len(self.dynamic) or offset + size > len(data):
                    raise ValueError("bad dynamic superblock definition")
                key = data[offset:offset + size]
                offset += size
                decode_key(key)
                self.dynamic.append(key)
                a = b = 0
            elif op == ExactEncoder.DYNAMIC_REF:
                dynamic_id, offset = get_varint(data, offset)
                if not 0 < dynamic_id < len(self.dynamic):
                    raise ValueError("unknown dynamic superblock")
                key = self.dynamic[dynamic_id]
                a = b = 0
            elif op == ExactEncoder.STATIC_ID:
                static_id, offset = get_varint(data, offset)
                if not 0 < static_id <= len(self.package.keys):
                    raise ValueError("unknown static superblock")
                key = self.package.keys[static_id - 1]
                a, b = b, static_id
            elif op == ExactEncoder.PREDICTED:
                if offset >= len(data):
                    raise ValueError("truncated prediction rank")
                rank = data[offset]
                offset += 1
                predicted = self.predictions.get((a, b), ())
                if rank >= len(predicted):
                    raise ValueError("unknown predicted superblock")
                static_id = predicted[rank]
                if not 0 < static_id <= len(self.package.keys):
                    raise ValueError("bad prediction table")
                key = self.package.keys[static_id - 1]
                a, b = b, static_id
            else:
                raise ValueError("unknown superblock opcode")
            keys.append(key)
        if offset != len(data):
            raise ValueError("trailing superblock bytes")
        out: list[tuple[int, int, int]] = []
        for key in keys:
            out.extend(decode_key(key))
        return out


@dataclasses.dataclass(slots=True)
class CodecResult:
    name: str
    budget: int
    static_assets: int
    static_definition_bytes: int
    prediction_bytes: int
    outer_dictionary_bytes: int = 0
    payload_bytes: int = 0
    wire_bytes: int = 0
    raw_bytes: int = 0
    tus: int = 0
    blocks: int = 0
    encode_seconds: float = 0.0
    decode_seconds: float = 0.0
    exact: bool = True
    fraction_curve: list[dict] = dataclasses.field(default_factory=list)

    def as_dict(self) -> dict:
        total_model = (self.static_definition_bytes + self.prediction_bytes +
                       self.outer_dictionary_bytes)
        return {
            **dataclasses.asdict(self),
            "model_bytes": total_model,
            "wire_plus_model": self.wire_bytes + total_model,
            "raw_input_ratio": self.raw_bytes / max(1, self.wire_bytes),
            "raw_input_ratio_charged": self.raw_bytes / max(1, self.wire_bytes + total_model),
            "encode_effective_GBps": self.raw_bytes / max(1e-12, self.encode_seconds) / 1e9,
            "decode_effective_GBps": self.raw_bytes / max(1e-12, self.decode_seconds) / 1e9,
        }


def evaluate_codec(path: str, name: str, budget: int, package: StaticPackage,
                   predictions: PredictionMap, level: int,
                   dictionary: zstd.ZstdCompressionDict | None = None) -> CodecResult:
    encoder = ExactEncoder(package, predictions)
    decoder = ExactDecoder(package, predictions)
    compressor = zstd.ZstdCompressor(level=level, dict_data=dictionary)
    decompressor = zstd.ZstdDecompressor(dict_data=dictionary)
    result = CodecResult(name, budget, len(package.keys), package.definition_bytes,
                         prediction_package_bytes(predictions),
                         len(dictionary.as_bytes()) if dictionary is not None else 0)
    per_tu: list[tuple[int, int]] = []
    for tu in iter_tus_fast(path):
        keys = materialization_keys(tu, package)
        begin = time.monotonic()
        payload = encoder.frame(keys)
        compressed = compressor.compress(payload)
        result.encode_seconds += time.monotonic() - begin
        begin = time.monotonic()
        recovered_payload = decompressor.decompress(compressed, max_output_size=len(payload))
        recovered = decoder.frame(recovered_payload)
        result.decode_seconds += time.monotonic() - begin
        exact = recovered_payload == payload and recovered == expected_regions(tu)
        result.exact &= exact
        if not exact:
            raise ValueError(f"{name}: exact replay failed at TU {tu.tu}")
        result.payload_bytes += len(payload)
        result.wire_bytes += len(compressed) + 4
        result.raw_bytes += tu.raw_bytes
        result.tus += 1
        result.blocks += len(keys)
        per_tu.append((tu.raw_bytes, len(compressed) + 4))
    raw_prefix = wire_prefix = 0
    threshold_index = 0
    thresholds = (0.10, 0.25, 0.50, 0.75, 1.00)
    for raw, wire in per_tu:
        raw_prefix += raw
        wire_prefix += wire
        while (threshold_index < len(thresholds) and
               raw_prefix >= result.raw_bytes * thresholds[threshold_index]):
            model_bytes = (result.static_definition_bytes + result.prediction_bytes +
                           result.outer_dictionary_bytes)
            result.fraction_curve.append({
                "fraction": thresholds[threshold_index],
                "raw_bytes": raw_prefix,
                "wire_bytes": wire_prefix,
                "wire_plus_model": wire_prefix + model_bytes,
                "raw_input_ratio_charged": raw_prefix / max(1, wire_prefix + model_bytes),
            })
            threshold_index += 1
    return result


def dictionary_samples(paths: Sequence[str], package: StaticPackage,
                       predictions: PredictionMap, maximum_samples: int,
                       maximum_bytes: int, seed: int) -> list[bytes]:
    reservoir: list[bytes] = []
    sampled_bytes = seen = 0
    rng = random.Random(seed)
    for path in paths:
        encoder = ExactEncoder(package, predictions)
        for tu in iter_tus_fast(path):
            sample = encoder.frame(materialization_keys(tu, package))
            if len(sample) > 65536:
                sample = sample[:32768] + sample[-32768:]
            seen += 1
            if len(reservoir) < maximum_samples and sampled_bytes + len(sample) <= maximum_bytes:
                reservoir.append(sample)
                sampled_bytes += len(sample)
                continue
            if not reservoir:
                continue
            slot = rng.randrange(seen)
            if slot < len(reservoir):
                replacement = sampled_bytes - len(reservoir[slot]) + len(sample)
                if replacement <= maximum_bytes:
                    sampled_bytes = replacement
                    reservoir[slot] = sample
    return reservoir


def token_windows(paths: Sequence[str], package: StaticPackage, width: int, capacity: int,
                  seed: int) -> np.ndarray:
    rng = random.Random(seed)
    windows: list[np.ndarray] = []
    seen = 0
    for path in paths:
        for tu in iter_tus_fast(path):
            tokens = [package.ids.get(key, 0) for key in materialization_keys(tu, package)]
            if len(tokens) < 2:
                continue
            # Overlapping random-alignment windows retain cross-block context without letting a
            # few giant TUs dominate the sample.
            start = rng.randrange(min(width, len(tokens) - 1))
            for offset in range(start, len(tokens) - 1, width):
                value = tokens[offset:offset + width + 1]
                if len(value) < 2:
                    continue
                if len(value) < width + 1:
                    value += [0] * (width + 1 - len(value))
                array = np.asarray(value, dtype=np.int64)
                seen += 1
                if len(windows) < capacity:
                    windows.append(array)
                else:
                    slot = rng.randrange(seen)
                    if slot < capacity:
                        windows[slot] = array
    if not windows:
        return np.empty((0, width + 1), np.int64)
    return np.stack(windows)


class TokenGRU(nn.Module):
    def __init__(self, vocabulary: int, embedding: int = 32, hidden: int = 96):
        super().__init__()
        self.embedding = nn.Embedding(vocabulary, embedding)
        self.body = nn.GRU(embedding, hidden, num_layers=2, batch_first=True)
        self.output = nn.Linear(hidden, vocabulary)

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        hidden, _ = self.body(self.embedding(value))
        return self.output(hidden)


class CausalBlock(nn.Module):
    def __init__(self, channels: int, dilation: int):
        super().__init__()
        self.padding = 2 * dilation
        self.conv = nn.Conv1d(channels, channels, 3, padding=self.padding, dilation=dilation)
        self.norm = nn.GELU()

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.norm(self.conv(value)[..., :-self.padding])


class TokenTCN(nn.Module):
    def __init__(self, vocabulary: int, embedding: int = 48, hidden: int = 96):
        super().__init__()
        self.embedding = nn.Embedding(vocabulary, embedding)
        self.input = nn.Conv1d(embedding, hidden, 1)
        self.blocks = nn.Sequential(*(CausalBlock(hidden, dilation) for dilation in (1, 2, 4, 8)))
        self.output = nn.Conv1d(hidden, vocabulary, 1)

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        hidden = self.input(self.embedding(value).transpose(1, 2))
        return self.output(self.blocks(hidden)).transpose(1, 2)


def parameters(model: nn.Module) -> int:
    return sum(value.numel() for value in model.parameters())


def train_teacher(model: nn.Module, train: np.ndarray, test: np.ndarray, epochs: int,
                  batch_size: int, threads: int, seed: int) -> tuple[dict, PredictionMap]:
    torch.manual_seed(seed)
    torch.set_num_threads(threads)
    train_x = torch.from_numpy(train[:, :-1])
    train_y = torch.from_numpy(train[:, 1:])
    test_x = torch.from_numpy(test[:, :-1])
    test_y = torch.from_numpy(test[:, 1:])
    loader = DataLoader(TensorDataset(train_x, train_y), batch_size=batch_size, shuffle=True)
    optimizer = torch.optim.AdamW(model.parameters(), lr=2e-3, weight_decay=1e-4)
    history: list[dict] = []
    model.train()
    for epoch in range(epochs):
        begin = time.monotonic()
        loss_sum = count = 0
        for x, y in loader:
            optimizer.zero_grad(set_to_none=True)
            logits = model(x)
            valid = y.flatten() != 0
            if not bool(valid.any()):
                continue
            loss = nn.functional.cross_entropy(logits.flatten(0, 1)[valid], y.flatten()[valid])
            loss.backward()
            optimizer.step()
            loss_sum += float(loss.detach()) * int(valid.sum())
            count += int(valid.sum())
        history.append({"epoch": epoch + 1, "loss": loss_sum / max(1, count),
                        "seconds": time.monotonic() - begin})

    recalls = {k: 0 for k in (1, 2, 4, 8)}
    known = total = 0
    votes: dict[Context, collections.Counter[int]] = collections.defaultdict(collections.Counter)
    begin = time.monotonic()
    model.eval()
    with torch.inference_mode():
        for offset in range(0, len(test_x), batch_size):
            x = test_x[offset:offset + batch_size]
            y = test_y[offset:offset + batch_size]
            logits = model(x)
            top = logits.topk(min(8, logits.shape[-1]), dim=-1).indices
            valid = y != 0
            known += int(valid.sum())
            total += y.numel()
            for k in recalls:
                recalls[k] += int(((top[..., :k] == y[..., None]).any(dim=-1) & valid).sum())

    # Distill on training contexts.  The decoder-visible selector is exactly the last two static
    # block ids; the teacher contributes only the voted top-K list stored in this immutable table.
    with torch.inference_mode():
        distill_limit = min(len(train_x), 20_000)
        for offset in range(0, distill_limit, batch_size):
            x = train_x[offset:offset + batch_size]
            logits = model(x)
            top = logits.topk(min(8, logits.shape[-1]), dim=-1).indices.cpu().numpy()
            actual = x.cpu().numpy()
            for row in range(len(actual)):
                a = b = 0
                for column in range(actual.shape[1]):
                    context = (a, b)
                    for rank, value in enumerate(top[row, column]):
                        if value:
                            votes[context][int(value)] += 8 - rank
                    token = int(actual[row, column])
                    if token:
                        a, b = b, token
                    else:
                        a = b = 0
    prediction = {context: tuple(value for value, _ in counter.most_common(8))
                  for context, counter in votes.items() if counter}
    seconds = time.monotonic() - begin
    metrics = {
        "parameters": parameters(model),
        "float_model_bytes": parameters(model) * 4,
        "history": history,
        "test_tokens": total,
        "known_test_tokens": known,
        "known_fraction": known / max(1, total),
        "recall": {str(k): recalls[k] / max(1, known) for k in recalls},
        "inference_seconds": seconds,
        "inference_tokens_per_second": total / max(1e-12, seconds),
        "distilled_contexts": len(prediction),
        "distilled_bytes": prediction_package_bytes(prediction),
    }
    return metrics, prediction


def run(args: argparse.Namespace) -> int:
    begin = time.monotonic()
    candidates, training_stats = collect_candidates(args.train, args.candidates,
                                                     args.phrase_lengths)
    result: dict = {
        "experiment": "disjoint_project_exact_pretrained_superblocks_and_teachers",
        "train": args.train,
        "test": args.test,
        "training": training_stats,
        "levels": [args.level],
        "rows": [],
    }
    packages = {budget: build_package(candidates, budget) for budget in args.budgets}

    # Dynamic define/ref is the exact zero-pretraining control.
    empty = package_from_keys([], 0)
    baseline = evaluate_codec(args.test, "dynamic-only", 0, empty, {}, args.level)
    result["rows"].append(baseline.as_dict())
    print(json.dumps({"row": baseline.as_dict()}), flush=True)

    prediction_maps: dict[int, PredictionMap] = {}
    for budget, package in packages.items():
        predictions = learn_ngram_map(args.train, package, args.topk, args.contexts)
        prediction_maps[budget] = predictions
        static = evaluate_codec(args.test, f"static-{budget}", budget, package, {}, args.level)
        ngram = evaluate_codec(args.test, f"static-ngram-{budget}", budget, package,
                               predictions, args.level)
        result["rows"].extend((static.as_dict(), ngram.as_dict()))
        print(json.dumps({"row": static.as_dict()}), flush=True)
        print(json.dumps({"row": ngram.as_dict()}), flush=True)

    if args.cdict_sizes:
        package = packages[args.cdict_budget]
        predictions = prediction_maps[args.cdict_budget]
        samples = dictionary_samples(args.train, package, predictions, args.cdict_samples,
                                     args.cdict_sample_bytes, args.seed ^ 0xCD1C7)
        result["outer_dictionary_training"] = {
            "package_budget": args.cdict_budget,
            "samples": len(samples),
            "sample_bytes": sum(map(len, samples)),
        }
        for size_kib in args.cdict_sizes:
            try:
                dictionary = zstd.train_dictionary(size_kib * 1024, samples)
            except zstd.ZstdError as error:
                result.setdefault("outer_dictionary_errors", []).append(
                    {"size_kib": size_kib, "error": str(error)})
                continue
            row = evaluate_codec(args.test,
                f"static-ngram-cdict-{args.cdict_budget}-{size_kib}KiB",
                args.cdict_budget, package, predictions, args.level, dictionary)
            result["rows"].append(row.as_dict())
            print(json.dumps({"row": row.as_dict()}), flush=True)

    teacher_budget = args.teacher_budget
    package = packages[teacher_budget]
    # A bounded neural vocabulary keeps this a tiny model.  Asset IDs are in MDL order, so the
    # vocabulary is a prefix of the exact package and IDs remain decoder-stable.
    if len(package.keys) > args.vocabulary:
        keys = package.keys[:args.vocabulary]
        package = package_from_keys(keys,
            sum(1 + varint_size(i + 1) + varint_size(len(key)) + len(key)
                for i, key in enumerate(keys)))
    train_windows = (token_windows(args.train, package, args.width, args.windows, args.seed)
                     if not args.skip_teachers else np.empty((0, args.width + 1), np.int64))
    test_windows = (token_windows([args.test], package, args.width, args.test_windows, args.seed ^ 1)
                    if not args.skip_teachers else np.empty((0, args.width + 1), np.int64))
    if not args.skip_teachers and len(train_windows) and len(test_windows):
        teachers: dict[str, dict] = {}
        for teacher_name, model in (("gru", TokenGRU(len(package.keys) + 1)),
                                    ("tcn", TokenTCN(len(package.keys) + 1))):
            metrics, distilled = train_teacher(model, train_windows, test_windows, args.epochs,
                                                args.batch, args.threads, args.seed)
            teachers[teacher_name] = metrics
            row = evaluate_codec(args.test, f"static-{teacher_name}-distilled-{teacher_budget}",
                                 teacher_budget, package, distilled, args.level)
            result["rows"].append(row.as_dict())
            print(json.dumps({"teacher": teacher_name, "metrics": metrics}), flush=True)
            print(json.dumps({"row": row.as_dict()}), flush=True)
        result["teachers"] = teachers
        result["teacher_dataset"] = {
            "train_windows": len(train_windows), "test_windows": len(test_windows),
            "width": args.width, "static_vocabulary": len(package.keys),
        }
    result["wall_seconds"] = time.monotonic() - begin
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser()
    p.add_argument("--train", nargs="+", required=True)
    p.add_argument("--test", required=True)
    p.add_argument("--budgets", nargs="+", type=int,
                   default=(0, 256 << 10, 1 << 20, 4 << 20, 16 << 20, 64 << 20))
    p.add_argument("--teacher-budget", type=int, default=4 << 20)
    p.add_argument("--candidates", type=int, default=750_000)
    p.add_argument("--phrase-lengths", nargs="+", type=int, default=(2, 4, 8, 16, 32))
    p.add_argument("--contexts", type=int, default=100_000)
    p.add_argument("--topk", type=int, default=8)
    p.add_argument("--vocabulary", type=int, default=4096)
    p.add_argument("--width", type=int, default=64)
    p.add_argument("--windows", type=int, default=40_000)
    p.add_argument("--test-windows", type=int, default=10_000)
    p.add_argument("--epochs", type=int, default=3)
    p.add_argument("--skip-teachers", action="store_true")
    p.add_argument("--batch", type=int, default=128)
    p.add_argument("--threads", type=int, default=12)
    p.add_argument("--level", type=int, default=1)
    p.add_argument("--cdict-budget", type=int, default=1 << 20)
    p.add_argument("--cdict-sizes", nargs="+", type=int, default=(64, 128, 256))
    p.add_argument("--cdict-samples", type=int, default=200_000)
    p.add_argument("--cdict-sample-bytes", type=int, default=256 << 20)
    p.add_argument("--seed", type=int, default=0x51B10C)
    p.add_argument("--report", required=True)
    return p


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
