#!/usr/bin/env python3
"""Issue #16 ML capability experiments over ml_bakeoff.cpp's exact event stream.

The C++ harness remains the byte authority.  This file supplies encoder-side proposal/ranking
models and residual-model oracles.  Candidate ranks can be replayed by the C++ decoder path; no
mutable learned state is required on the receiving side for ranking experiments.
"""

from __future__ import annotations

import argparse
import dataclasses
import io
import json
import math
import os
import random
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, Sequence

import numpy as np
import torch
import zstandard as zstd
from torch import nn
from torch.utils.data import DataLoader, Dataset


U8 = struct.Struct("<B")
U32 = struct.Struct("<I")
U64 = struct.Struct("<Q")
TU_HEAD = struct.Struct("<IQI")
REGION = struct.Struct("<QQQI")
EVENT_FIXED = struct.Struct("<IIIIIIIIQQQQ")
CANDIDATE = struct.Struct("<IBBHIIIIIIif")
FOOTER = struct.Struct("<QQQQQ")


def read_exact(f: BinaryIO, n: int) -> bytes:
    data = f.read(n)
    if len(data) != n:
        raise EOFError(f"wanted {n} bytes, got {len(data)}")
    return data


@dataclasses.dataclass(slots=True)
class TuRecord:
    tu: int
    raw_bytes: int
    regions: list[tuple[int, int, int, int]]  # hash1, hash2, block context, raw length


@dataclasses.dataclass(slots=True)
class Candidate:
    base_id: int
    sources: int
    age: int
    prefix: int
    suffix: int
    qgrams: int
    prefix_cost: int
    program_cost: int
    reward: int
    ftrl_score: float

    @property
    def cost(self) -> int:
        return min(self.prefix_cost, self.program_cost)


@dataclasses.dataclass(slots=True)
class Event:
    tu: int
    line_id: int
    path_id: int
    logical_line: int
    marker_flags: int
    previous_line: int
    previous2_line: int
    region_ordinal: int
    region_hash: int
    block_context: int
    skeleton: int
    signature: int
    line: bytes
    candidates: list[Candidate]


@dataclasses.dataclass(slots=True)
class Footer:
    raw_bytes: int
    line_bytes: int
    tus: int
    events: int
    candidates: int


Record = TuRecord | Event | Footer


def iter_dataset(path: str | os.PathLike[str]) -> Iterator[Record]:
    with open(path, "rb") as f:
        if read_exact(f, 8) != b"ICMLDS2\0":
            raise ValueError(f"{path}: wrong dataset magic")
        version = U32.unpack(read_exact(f, U32.size))[0]
        if version != 2:
            raise ValueError(f"{path}: unsupported schema {version}")
        while True:
            tag = U8.unpack(read_exact(f, 1))[0]
            if tag == 0:
                yield Footer(*FOOTER.unpack(read_exact(f, FOOTER.size)))
                if f.read(1):
                    raise ValueError(f"{path}: trailing bytes")
                return
            if tag == 1:
                tu, raw_bytes, count = TU_HEAD.unpack(read_exact(f, TU_HEAD.size))
                regions = [REGION.unpack(read_exact(f, REGION.size)) for _ in range(count)]
                yield TuRecord(tu, raw_bytes, regions)
                continue
            if tag != 2:
                raise ValueError(f"{path}: unknown record tag {tag}")
            fixed = EVENT_FIXED.unpack(read_exact(f, EVENT_FIXED.size))
            line_len = U32.unpack(read_exact(f, U32.size))[0]
            line = read_exact(f, line_len)
            count = U32.unpack(read_exact(f, U32.size))[0]
            candidates: list[Candidate] = []
            for _ in range(count):
                raw = CANDIDATE.unpack(read_exact(f, CANDIDATE.size))
                candidates.append(Candidate(raw[0], raw[1], raw[4], raw[5], raw[6], raw[7],
                                            raw[8], raw[9], raw[10], raw[11]))
            yield Event(*fixed, line, candidates)


@dataclasses.dataclass(slots=True)
class LineMeta:
    path_id: int
    logical_line: int
    marker_flags: int
    region_hash: int
    block_context: int
    skeleton: int
    signature: int


def scalar_features(event: Event, candidate: Candidate, base: bytes, base_meta: LineMeta) -> np.ndarray:
    target_len = len(event.line)
    base_len = len(base)
    source_bits = [(candidate.sources >> i) & 1 for i in range(6)]
    values = source_bits + [
        math.log2(target_len + 1) / 16.0,
        math.log2(base_len + 1) / 16.0,
        math.log2(candidate.age + 1) / 24.0,
        candidate.prefix / max(1, target_len),
        candidate.suffix / max(1, target_len),
        min(candidate.qgrams, 8) / 8.0,
        max(-4.0, min(4.0, (target_len - base_len) / 64.0)) / 4.0,
        float(event.path_id == base_meta.path_id),
        float(event.logical_line == base_meta.logical_line),
        float(event.marker_flags == base_meta.marker_flags),
        float(event.skeleton == base_meta.skeleton),
        float(event.signature == base_meta.signature),
        float(event.region_hash == base_meta.region_hash),
        float(event.block_context == base_meta.block_context),
        (event.block_context & 0xffff) / 65535.0,
        (event.region_hash & 0xffff) / 65535.0,
    ]
    return np.asarray(values, dtype=np.float32)


SCALAR_DIM = 22


@dataclasses.dataclass(slots=True)
class Sample:
    target: bytes
    base: bytes
    scalars: np.ndarray
    reward: float


class Reservoir:
    def __init__(self, capacity: int, seed: int = 0x1CE50):
        self.capacity = capacity
        self.items: list[Sample] = []
        self.seen = 0
        self.random = random.Random(seed)

    def add(self, item: Sample) -> None:
        self.seen += 1
        if len(self.items) < self.capacity:
            self.items.append(item)
            return
        slot = self.random.randrange(self.seen)
        if slot < self.capacity:
            self.items[slot] = item


def collect_samples(paths: Sequence[str], capacity: int) -> tuple[list[Sample], dict[str, int]]:
    reservoir = Reservoir(capacity)
    stats = defaultdict(int)
    for path in paths:
        lines: list[bytes] = [b""]
        metadata: list[LineMeta | None] = [None]
        for record in iter_dataset(path):
            if isinstance(record, Event):
                if record.line_id != len(lines):
                    raise ValueError(f"{path}: non-sequential line id {record.line_id}/{len(lines)}")
                for candidate in record.candidates:
                    if not 0 < candidate.base_id < len(lines):
                        raise ValueError(f"{path}: invalid base {candidate.base_id}")
                    base_meta = metadata[candidate.base_id]
                    assert base_meta is not None
                    reservoir.add(Sample(record.line, lines[candidate.base_id],
                                         scalar_features(record, candidate, lines[candidate.base_id], base_meta),
                                         float(candidate.reward)))
                    stats["candidates"] += 1
                lines.append(record.line)
                metadata.append(LineMeta(record.path_id, record.logical_line, record.marker_flags,
                                         record.region_hash, record.block_context,
                                         record.skeleton, record.signature))
                stats["events"] += 1
            elif isinstance(record, Footer):
                stats["raw_bytes"] += record.raw_bytes
                stats["line_bytes"] += record.line_bytes
    stats["sampled"] = len(reservoir.items)
    stats["seen"] = reservoir.seen
    return reservoir.items, dict(stats)


def encode_bytes(value: bytes, width: int) -> tuple[np.ndarray, int]:
    clipped = value[:width]
    out = np.zeros(width, dtype=np.int64)
    if clipped:
        out[:len(clipped)] = np.frombuffer(clipped, dtype=np.uint8).astype(np.int64) + 1
    return out, len(clipped)


class PairDataset(Dataset):
    def __init__(self, samples: Sequence[Sample], width: int):
        self.samples = samples
        self.width = width

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int):
        sample = self.samples[index]
        target, target_len = encode_bytes(sample.target, self.width)
        base, base_len = encode_bytes(sample.base, self.width)
        # Robust scaling: byte rewards are heavy-tailed because a few generated lines are huge.
        reward = np.float32(np.sign(sample.reward) * math.log1p(abs(sample.reward)) / 8.0)
        return target, target_len, base, base_len, sample.scalars, reward


class ByteTower(nn.Module):
    def __init__(self, embedding: int = 24, hidden: int = 64):
        super().__init__()
        self.embedding = nn.Embedding(257, embedding, padding_idx=0)
        self.net = nn.Sequential(
            nn.Conv1d(embedding, hidden, 5, padding=2), nn.GELU(),
            nn.Conv1d(hidden, hidden, 3, padding=1), nn.GELU(),
        )

    def forward(self, tokens: torch.Tensor, lengths: torch.Tensor) -> torch.Tensor:
        x = self.net(self.embedding(tokens).transpose(1, 2))
        positions = torch.arange(tokens.shape[1], device=tokens.device)[None, :]
        mask = positions < lengths[:, None]
        neg = torch.finfo(x.dtype).min
        maximum = x.masked_fill(~mask[:, None, :], neg).amax(dim=2)
        summed = (x * mask[:, None, :]).sum(dim=2)
        mean = summed / lengths.clamp_min(1)[:, None]
        return torch.cat((mean, maximum), dim=1)


class PairRanker(nn.Module):
    def __init__(self, scalar_dim: int = SCALAR_DIM, hidden: int = 64):
        super().__init__()
        self.tower = ByteTower(hidden=hidden)
        tower_out = hidden * 2
        self.head = nn.Sequential(
            nn.Linear(tower_out * 3 + scalar_dim, 128), nn.GELU(),
            nn.Linear(128, 64), nn.GELU(), nn.Linear(64, 1),
        )

    def forward(self, target, target_len, base, base_len, scalars):
        t = self.tower(target, target_len)
        b = self.tower(base, base_len)
        return self.head(torch.cat((t, b, torch.abs(t - b), scalars), dim=1)).squeeze(1)


class DualEncoderRanker(nn.Module):
    """Context/target-to-base retriever; dot-product ranking permits a future ANN index."""

    def __init__(self, scalar_dim: int = SCALAR_DIM, hidden: int = 64, output: int = 96):
        super().__init__()
        self.tower = ByteTower(hidden=hidden)
        tower_out = hidden * 2
        self.query = nn.Sequential(nn.Linear(tower_out + scalar_dim, 128), nn.GELU(),
                                   nn.Linear(128, output))
        self.document = nn.Sequential(nn.Linear(tower_out + scalar_dim, 128), nn.GELU(),
                                      nn.Linear(128, output))
        self.bias = nn.Sequential(nn.Linear(scalar_dim, 32), nn.GELU(), nn.Linear(32, 1))

    def forward(self, target, target_len, base, base_len, scalars):
        target_value = self.tower(target, target_len)
        base_value = self.tower(base, base_len)
        query = nn.functional.normalize(self.query(torch.cat((target_value, scalars), dim=1)), dim=1)
        document = nn.functional.normalize(self.document(torch.cat((base_value, scalars), dim=1)), dim=1)
        return (query * document).sum(dim=1) * 4.0 + self.bias(scalars).squeeze(1)


class MetadataRanker(nn.Module):
    """Small source-location/context variant selector without byte-tower inference."""

    def __init__(self, scalar_dim: int = SCALAR_DIM):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(scalar_dim, 96), nn.GELU(),
                                 nn.Linear(96, 48), nn.GELU(), nn.Linear(48, 1))

    def forward(self, target, target_len, base, base_len, scalars):
        del target, target_len, base, base_len
        return self.net(scalars).squeeze(1)


def model_size(model: nn.Module) -> int:
    return sum(p.numel() * p.element_size() for p in model.parameters())


def serialized_state_bytes(model: nn.Module) -> int:
    buffer = io.BytesIO()
    torch.save(model.state_dict(), buffer)
    return buffer.tell()


def train_ranker(samples: Sequence[Sample], width: int, epochs: int, batch: int,
                 threads: int, seed: int) -> tuple[PairRanker, list[dict[str, float]]]:
    torch.manual_seed(seed)
    torch.set_num_threads(threads)
    model = PairRanker()
    loader = DataLoader(PairDataset(samples, width), batch_size=batch, shuffle=True,
                        num_workers=0, drop_last=False)
    optimizer = torch.optim.AdamW(model.parameters(), lr=2e-3, weight_decay=1e-4)
    loss_fn = nn.SmoothL1Loss()
    history: list[dict[str, float]] = []
    model.train()
    for epoch in range(epochs):
        begin = time.monotonic()
        total_loss = 0.0
        count = 0
        for target, target_len, base, base_len, scalars, reward in loader:
            optimizer.zero_grad(set_to_none=True)
            prediction = model(target, target_len, base, base_len, scalars)
            loss = loss_fn(prediction, reward)
            loss.backward()
            optimizer.step()
            total_loss += float(loss) * len(target)
            count += len(target)
        row = {"epoch": epoch + 1, "loss": total_loss / max(1, count),
               "seconds": time.monotonic() - begin}
        history.append(row)
        print(json.dumps({"train": row}), flush=True)
    return model, history


def predict_samples(model: nn.Module, samples: Sequence[Sample], width: int,
                    batch: int) -> np.ndarray:
    loader = DataLoader(PairDataset(samples, width), batch_size=batch, shuffle=False,
                        num_workers=0, drop_last=False)
    predictions: list[np.ndarray] = []
    model.eval()
    with torch.inference_mode():
        for target, target_len, base, base_len, scalars, _ in loader:
            predictions.append(model(target, target_len, base, base_len, scalars).cpu().numpy())
    return np.concatenate(predictions) if predictions else np.empty(0, np.float32)


def train_model(model: nn.Module, samples: Sequence[Sample], width: int, epochs: int,
                batch: int, threads: int, seed: int) -> list[dict[str, float]]:
    torch.manual_seed(seed)
    torch.set_num_threads(threads)
    loader = DataLoader(PairDataset(samples, width), batch_size=batch, shuffle=True,
                        num_workers=0, drop_last=False)
    optimizer = torch.optim.AdamW(model.parameters(), lr=2e-3, weight_decay=1e-4)
    loss_fn = nn.SmoothL1Loss()
    history: list[dict[str, float]] = []
    model.train()
    for epoch in range(epochs):
        begin = time.monotonic()
        total_loss = 0.0
        count = 0
        for target, target_len, base, base_len, scalars, reward in loader:
            optimizer.zero_grad(set_to_none=True)
            prediction = model(target, target_len, base, base_len, scalars)
            loss = loss_fn(prediction, reward)
            loss.backward()
            optimizer.step()
            total_loss += float(loss.detach()) * len(target)
            count += len(target)
        history.append({"epoch": epoch + 1, "loss": total_loss / max(1, count),
                        "seconds": time.monotonic() - begin})
    return history


@dataclasses.dataclass(slots=True)
class EventGroup:
    samples: list[Sample]
    costs: np.ndarray


class GroupReservoir:
    def __init__(self, capacity: int, seed: int):
        self.capacity = capacity
        self.items: list[EventGroup] = []
        self.seen = 0
        self.random = random.Random(seed)

    def add(self, value: EventGroup) -> None:
        self.seen += 1
        if len(self.items) < self.capacity:
            self.items.append(value)
            return
        slot = self.random.randrange(self.seen)
        if slot < self.capacity:
            self.items[slot] = value


def collect_event_groups(path: str, capacity: int, seed: int) -> tuple[list[EventGroup], dict]:
    reservoir = GroupReservoir(capacity, seed)
    lines: list[bytes] = [b""]
    metadata: list[LineMeta | None] = [None]
    stats = defaultdict(int)
    for record in iter_dataset(path):
        if isinstance(record, TuRecord):
            stats["raw_bytes"] += record.raw_bytes
            stats["tus"] += 1
        elif isinstance(record, Event):
            if record.line_id != len(lines):
                raise ValueError(f"{path}: non-sequential line id")
            samples = event_samples(record, lines, metadata)
            if samples:
                reservoir.add(EventGroup(samples,
                    np.asarray([candidate.cost for candidate in record.candidates], np.int64)))
                stats["events_with_candidates"] += 1
                stats["candidate_rows"] += len(samples)
            lines.append(record.line)
            metadata.append(LineMeta(record.path_id, record.logical_line, record.marker_flags,
                                     record.region_hash, record.block_context,
                                     record.skeleton, record.signature))
    stats["sampled_events"] = len(reservoir.items)
    stats["events_seen_by_reservoir"] = reservoir.seen
    return reservoir.items, dict(stats)


def evaluate_group_sample(model: nn.Module, groups: Sequence[EventGroup], width: int,
                          batch: int) -> dict:
    samples: list[Sample] = []
    spans: list[tuple[int, int]] = []
    for group in groups:
        begin = len(samples)
        samples.extend(group.samples)
        spans.append((begin, len(samples)))
    begin = time.monotonic()
    predictions = predict_samples(model, samples, width, batch)
    seconds = time.monotonic() - begin
    recall = {k: 0 for k in (1, 2, 4, 8)}
    regret = {k: 0 for k in (1, 2, 4, 8)}
    for group, (first, last) in zip(groups, spans):
        order = np.argsort(-predictions[first:last], kind="stable")
        best = int(group.costs.min())
        best_index = int(group.costs.argmin())
        for k in recall:
            selected = order[:min(k, len(order))]
            recall[k] += int(best_index in selected)
            regret[k] += int(group.costs[selected].min()) - best
    count = len(groups)
    return {
        "sampled_events": count,
        "candidate_rows": len(samples),
        "recall": {str(k): recall[k] / max(1, count) for k in recall},
        "total_byte_regret": {str(k): regret[k] for k in regret},
        "byte_regret_per_event": {str(k): regret[k] / max(1, count) for k in regret},
        "inference_seconds": seconds,
        "candidate_rows_per_second": len(samples) / max(1e-12, seconds),
    }


class RankWriter:
    def __init__(self, path: str | None):
        self.f = open(path, "wb") if path else None
        if self.f:
            self.f.write(b"ICRANK1\0")
            self.f.write(U32.pack(1))
            self.count = 0

    def write(self, event: Event, order: np.ndarray) -> None:
        if not self.f:
            return
        top = list(map(int, order[:8]))
        self.f.write(struct.pack("<IIHH", event.tu, event.line_id, len(event.candidates), len(top)))
        self.f.write(struct.pack("<" + "H" * len(top), *top))
        self.count += 1

    def close(self) -> None:
        if self.f:
            self.f.write(struct.pack("<BQ", 0, self.count))
            self.f.close()


def event_samples(event: Event, lines: list[bytes], metadata: list[LineMeta | None]) -> list[Sample]:
    out: list[Sample] = []
    for candidate in event.candidates:
        base_meta = metadata[candidate.base_id]
        assert base_meta is not None
        out.append(Sample(event.line, lines[candidate.base_id],
                          scalar_features(event, candidate, lines[candidate.base_id], base_meta),
                          float(candidate.reward)))
    return out


def evaluate_ranker(model: nn.Module, path: str, width: int, batch: int,
                    ranks_path: str | None, online: bool, online_lr: float) -> dict:
    lines: list[bytes] = [b""]
    metadata: list[LineMeta | None] = [None]
    writer = RankWriter(ranks_path)
    k_values = (1, 2, 4, 8)
    recall = np.zeros(len(k_values), np.int64)
    regret = np.zeros(len(k_values), np.int64)
    events_with_candidates = 0
    candidate_count = 0
    raw_bytes = 0
    tu_events: list[Event] = []
    optimizer = torch.optim.SGD(model.parameters(), lr=online_lr) if online else None
    loss_fn = nn.SmoothL1Loss()

    def score_tu(events: list[Event]) -> None:
        nonlocal events_with_candidates, candidate_count
        if not events:
            return
        samples: list[Sample] = []
        spans: list[tuple[int, int]] = []
        for event in events:
            begin = len(samples)
            samples.extend(event_samples(event, lines, metadata))
            spans.append((begin, len(samples)))
        prediction = predict_samples(model, samples, width, batch)
        for event, (begin, end) in zip(events, spans):
            candidate_count += len(event.candidates)
            if begin == end:
                writer.write(event, np.empty(0, np.int64))
                continue
            scores = prediction[begin:end]
            order = np.argsort(-scores, kind="stable")
            costs = np.asarray([c.cost for c in event.candidates], np.int64)
            best = int(np.argmin(costs))
            best_cost = int(costs[best])
            events_with_candidates += 1
            for i, k in enumerate(k_values):
                selected = order[:min(k, len(order))]
                recall[i] += int(best in selected)
                regret[i] += int(costs[selected].min()) - best_cost
            writer.write(event, order)

        # Strict prequential ordering: all predictions above use the model as it stood before TU.
        if optimizer is not None and samples:
            model.train()
            loader = DataLoader(PairDataset(samples, width), batch_size=batch, shuffle=True,
                                num_workers=0)
            for target, target_len, base, base_len, scalars, reward in loader:
                optimizer.zero_grad(set_to_none=True)
                pred = model(target, target_len, base, base_len, scalars)
                loss = loss_fn(pred, reward)
                loss.backward()
                optimizer.step()
            model.eval()

    current_tu: int | None = None
    begin = time.monotonic()
    for record in iter_dataset(path):
        if isinstance(record, TuRecord):
            if current_tu is not None:
                score_tu(tu_events)
                tu_events.clear()
            current_tu = record.tu
            raw_bytes += record.raw_bytes
        elif isinstance(record, Event):
            if record.line_id != len(lines):
                raise ValueError(f"{path}: non-sequential line id")
            # Base lookup for this TU must see prior lines and earlier definitions in this TU.
            tu_events.append(record)
            lines.append(record.line)
            metadata.append(LineMeta(record.path_id, record.logical_line, record.marker_flags,
                                     record.region_hash, record.block_context,
                                     record.skeleton, record.signature))
        elif isinstance(record, Footer):
            score_tu(tu_events)
            tu_events.clear()
    writer.close()
    seconds = time.monotonic() - begin
    return {
        "dataset": path,
        "raw_bytes": raw_bytes,
        "events_with_candidates": events_with_candidates,
        "candidate_rows": candidate_count,
        "recall": {str(k): float(recall[i] / max(1, events_with_candidates))
                   for i, k in enumerate(k_values)},
        "total_byte_regret": {str(k): int(regret[i]) for i, k in enumerate(k_values)},
        "byte_regret_per_event": {str(k): float(regret[i] / max(1, events_with_candidates))
                                  for i, k in enumerate(k_values)},
        "seconds": seconds,
        "effective_raw_GBps": raw_bytes / max(seconds, 1e-9) / 1e9,
        "online_updates": online,
    }


def ranker_command(args: argparse.Namespace) -> int:
    begin = time.monotonic()
    samples, sample_stats = collect_samples(args.train, args.samples)
    print(json.dumps({"sample_stats": sample_stats}), flush=True)
    model, history = train_ranker(samples, args.width, args.epochs, args.batch,
                                  args.threads, args.seed)
    float_bytes = model_size(model)
    quantized = torch.ao.quantization.quantize_dynamic(model, {nn.Linear}, dtype=torch.qint8)
    result = {
        "experiment": "tiny_shared_tcn_candidate_ranker",
        "train": args.train,
        "test": args.test,
        "sample_stats": sample_stats,
        "history": history,
        "parameters": sum(p.numel() for p in model.parameters()),
        "float_model_bytes": float_bytes,
        "dynamic_quantized_state_bytes": serialized_state_bytes(quantized),
        "width": args.width,
    }
    torch.save({"model": model.state_dict(), "config": result}, args.model_out)
    result["fixed_pretrained"] = evaluate_ranker(model, args.test, args.width, args.batch,
                                                  args.ranks_out, False, args.online_lr)
    if args.online:
        online_model = PairRanker()
        online_model.load_state_dict(model.state_dict())
        result["pretrained_plus_online"] = evaluate_ranker(
            online_model, args.test, args.width, args.batch,
            args.online_ranks_out, True, args.online_lr)
    result["wall_seconds"] = time.monotonic() - begin
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def neural_suite_command(args: argparse.Namespace) -> int:
    """Compare all encoder-side neural proposals on one shared candidate-cost sample."""
    begin = time.monotonic()
    samples, sample_stats = collect_samples(args.train, args.samples)
    groups, test_stats = collect_event_groups(args.test, args.test_events, args.seed ^ 0x55AA)
    result: dict = {
        "experiment": "candidate_ml_suite_pair_dual_source_context",
        "train": args.train,
        "test": args.test,
        "sample_stats": sample_stats,
        "test_stats": test_stats,
        "models": {},
    }
    models: list[tuple[str, nn.Module]] = [
        ("shared_byte_cnn_pair_ranker", PairRanker()),
        ("context_to_base_dual_encoder", DualEncoderRanker()),
        ("source_location_context_mlp", MetadataRanker()),
    ]
    saved: dict[str, dict] = {}
    for index, (name, model) in enumerate(models):
        history = train_model(model, samples, args.width, args.epochs, args.batch,
                              args.threads, args.seed + index)
        metrics = evaluate_group_sample(model, groups, args.width, args.batch)
        quantized = torch.ao.quantization.quantize_dynamic(model, {nn.Linear}, dtype=torch.qint8)
        row = {
            "parameters": sum(parameter.numel() for parameter in model.parameters()),
            "float_model_bytes": model_size(model),
            "dynamic_quantized_state_bytes": serialized_state_bytes(quantized),
            "history": history,
            "heldout": metrics,
            "decoder_requires_ranker": False,
        }
        result["models"][name] = row
        saved[name] = model.state_dict()
        print(json.dumps({"model": name, "result": row}), flush=True)
    torch.save({"models": saved, "config": result}, args.model_out)
    result["wall_seconds"] = time.monotonic() - begin
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def evaluate_gbdt(model, path: str, batch_rows: int) -> dict:
    lines: list[bytes] = [b""]
    metadata: list[LineMeta | None] = [None]
    features: list[np.ndarray] = []
    groups: list[np.ndarray] = []
    spans: list[tuple[int, int]] = []
    recall = {k: 0 for k in (1, 2, 4, 8)}
    regret = {k: 0 for k in (1, 2, 4, 8)}
    events = candidates = raw_bytes = tus = 0
    predict_seconds = 0.0

    def flush() -> None:
        nonlocal predict_seconds, events, candidates
        if not features:
            return
        matrix = np.stack(features)
        begin = time.monotonic()
        prediction = model.booster_.predict(matrix)
        predict_seconds += time.monotonic() - begin
        for costs, (first, last) in zip(groups, spans):
            order = np.argsort(-prediction[first:last], kind="stable")
            best_index = int(costs.argmin())
            best = int(costs[best_index])
            for k in recall:
                selected = order[:min(k, len(order))]
                recall[k] += int(best_index in selected)
                regret[k] += int(costs[selected].min()) - best
            events += 1
            candidates += len(costs)
        features.clear()
        groups.clear()
        spans.clear()

    scan_begin = time.monotonic()
    for record in iter_dataset(path):
        if isinstance(record, TuRecord):
            raw_bytes += record.raw_bytes
            tus += 1
        elif isinstance(record, Event):
            if record.line_id != len(lines):
                raise ValueError(f"{path}: non-sequential line id")
            samples = event_samples(record, lines, metadata)
            if samples:
                first = len(features)
                features.extend(sample.scalars for sample in samples)
                spans.append((first, len(features)))
                groups.append(np.asarray([candidate.cost for candidate in record.candidates],
                                         np.int64))
            lines.append(record.line)
            metadata.append(LineMeta(record.path_id, record.logical_line, record.marker_flags,
                                     record.region_hash, record.block_context,
                                     record.skeleton, record.signature))
            if len(features) >= batch_rows:
                flush()
    flush()
    scan_seconds = time.monotonic() - scan_begin
    return {
        "dataset": path,
        "tus": tus,
        "raw_bytes": raw_bytes,
        "events_with_candidates": events,
        "candidate_rows": candidates,
        "recall": {str(k): recall[k] / max(1, events) for k in recall},
        "total_byte_regret": {str(k): regret[k] for k in regret},
        "byte_regret_per_event": {str(k): regret[k] / max(1, events) for k in regret},
        "predict_seconds": predict_seconds,
        "candidate_rows_per_predict_second": candidates / max(1e-12, predict_seconds),
        "effective_raw_GBps_predict_only": raw_bytes / max(1e-12, predict_seconds) / 1e9,
        "complete_scan_seconds": scan_seconds,
        "complete_scan_raw_GBps": raw_bytes / max(1e-12, scan_seconds) / 1e9,
    }


def gbdt_command(args: argparse.Namespace) -> int:
    """Fixed pretrained source/context cost ranker; decoder receives only the chosen program."""
    import lightgbm as lgb

    begin = time.monotonic()
    samples, sample_stats = collect_samples(args.train, args.samples)
    x = np.stack([sample.scalars for sample in samples])
    reward = np.asarray([math.copysign(math.log1p(abs(sample.reward)), sample.reward) / 8.0
                         for sample in samples], np.float32)
    model = lgb.LGBMRegressor(
        objective="huber", n_estimators=args.trees, learning_rate=args.learning_rate,
        num_leaves=args.leaves, max_depth=args.depth, min_child_samples=64,
        feature_fraction=0.9, bagging_fraction=0.9, bagging_freq=1,
        reg_lambda=1e-3, n_jobs=args.threads, random_state=args.seed, verbosity=-1,
    )
    train_begin = time.monotonic()
    model.fit(x, reward)
    train_seconds = time.monotonic() - train_begin
    model_text = model.booster_.model_to_string()
    Path(args.model_out).write_text(model_text)
    heldout = evaluate_gbdt(model, args.test, args.batch_rows)
    result = {
        "experiment": "pretrained_gbdt_source_location_variant_cost_ranker",
        "train": args.train,
        "test": args.test,
        "sample_stats": sample_stats,
        "trees": args.trees,
        "leaves": args.leaves,
        "depth": args.depth,
        "model_bytes": len(model_text.encode()),
        "train_seconds": train_seconds,
        "heldout": heldout,
        "decoder_requires_ranker": False,
        "wall_seconds": time.monotonic() - begin,
    }
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def inspect_command(args: argparse.Namespace) -> int:
    counts = defaultdict(int)
    begin = time.monotonic()
    for record in iter_dataset(args.dataset):
        counts[type(record).__name__] += 1
        if isinstance(record, Event):
            counts["candidate_rows"] += len(record.candidates)
            counts["line_bytes"] += len(record.line)
        elif isinstance(record, TuRecord):
            counts["raw_bytes"] += record.raw_bytes
            counts["regions"] += len(record.regions)
        elif isinstance(record, Footer):
            counts["footer_events"] = record.events
            counts["footer_candidates"] = record.candidates
    counts["seconds"] = time.monotonic() - begin
    print(json.dumps(dict(counts), indent=2, sort_keys=True))
    return 0


def put_varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7f) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def grouped_tus(path: str) -> Iterator[tuple[TuRecord, list[Event]]]:
    current: TuRecord | None = None
    events: list[Event] = []
    for record in iter_dataset(path):
        if isinstance(record, TuRecord):
            if current is not None:
                yield current, events
            current = record
            events = []
        elif isinstance(record, Event):
            if current is None or record.tu != current.tu:
                raise ValueError(f"{path}: event outside its TU")
            events.append(record)
        elif isinstance(record, Footer):
            if current is not None:
                yield current, events
            return


def literal_record(event: Event) -> bytes:
    return b"\x00" + put_varint(event.line_id) + put_varint(len(event.line)) + event.line


def definition_frame(events: Sequence[Event]) -> bytes:
    return put_varint(len(events)) + b"".join(literal_record(event) for event in events)


def root_digest_frame(tu: TuRecord) -> bytes:
    out = bytearray(put_varint(len(tu.regions)))
    for hash1, hash2, _, raw_len in tu.regions:
        out += struct.pack("<QQI", hash1, hash2, raw_len)
    return bytes(out)


class DynamicSuperblocks:
    """Self-describing exact region-digest block stream used for the pretraining control."""

    def __init__(self) -> None:
        self.ids: dict[tuple[tuple[int, int, int], ...], int] = {}

    def frame(self, tu: TuRecord) -> bytes:
        runs: list[tuple[tuple[int, int, int], ...]] = []
        i = 0
        while i < len(tu.regions):
            context = tu.regions[i][2]
            j = i + 1
            while j < len(tu.regions) and tu.regions[j][2] == context:
                j += 1
            runs.append(tuple((r[0], r[1], r[3]) for r in tu.regions[i:j]))
            i = j
        out = bytearray(put_varint(len(runs)))
        for run in runs:
            block_id = self.ids.get(run)
            if block_id is None:
                block_id = len(self.ids)
                self.ids[run] = block_id
                out.append(0)  # DEFINE_AND_USE
                out += put_varint(block_id)
                out += put_varint(len(run))
                for hash1, hash2, raw_len in run:
                    out += struct.pack("<QQI", hash1, hash2, raw_len)
            else:
                out.append(1)  # REF
                out += put_varint(block_id)
        return bytes(out)


def stream_frames(path: str, stream: str) -> tuple[list[bytes], int, int]:
    frames: list[bytes] = []
    raw_input = 0
    payload = 0
    blocks = DynamicSuperblocks()
    for tu, events in grouped_tus(path):
        raw_input += tu.raw_bytes
        if stream == "definition":
            frame = definition_frame(events)
        elif stream == "root-digest":
            frame = root_digest_frame(tu)
        elif stream == "superblock":
            frame = blocks.frame(tu)
        else:
            raise ValueError(stream)
        if (stream == "definition" and events) or (stream != "definition" and tu.regions):
            frames.append(frame)
            payload += len(frame)
    return frames, raw_input, payload


def training_samples(paths: Sequence[str], stream: str, limit: int,
                     byte_limit: int, seed: int) -> tuple[list[bytes], dict[str, int]]:
    rng = random.Random(seed)
    reservoir: list[bytes] = []
    seen = 0
    sampled_bytes = 0
    # Definition dictionaries learn more reliably from individual exact records; structure
    # dictionaries learn from complete TU frames so repeated digest/token sequences remain intact.
    for path in paths:
        if stream == "definition":
            iterator: Iterable[bytes] = (
                literal_record(event)
                for _, events in grouped_tus(path)
                for event in events
            )
        else:
            iterator = stream_frames(path, stream)[0]
        for sample in iterator:
            if not sample:
                continue
            # zstd's trainer does not benefit from giant single samples; retain both ends of long
            # records so the sample budget remains representative.
            if len(sample) > 65536:
                sample = sample[:32768] + sample[-32768:]
            seen += 1
            if len(reservoir) < limit and sampled_bytes + len(sample) <= byte_limit:
                reservoir.append(sample)
                sampled_bytes += len(sample)
            else:
                slot = rng.randrange(seen)
                if slot < len(reservoir):
                    sampled_bytes -= len(reservoir[slot])
                    if sampled_bytes + len(sample) <= byte_limit:
                        reservoir[slot] = sample
                        sampled_bytes += len(sample)
    return reservoir, {"seen": seen, "samples": len(reservoir), "bytes": sampled_bytes}


def evaluate_dictionary(frames: Sequence[bytes], raw_input: int, payload: int,
                        dictionary: zstd.ZstdCompressionDict | None, level: int) -> dict:
    compressor = zstd.ZstdCompressor(level=level, dict_data=dictionary)
    decompressor = zstd.ZstdDecompressor(dict_data=dictionary)
    encoded: list[bytes] = []
    begin = time.monotonic()
    for frame in frames:
        encoded.append(compressor.compress(frame))
    encode_seconds = time.monotonic() - begin
    wire = sum(len(value) + 4 for value in encoded)
    begin = time.monotonic()
    exact = True
    for source, compressed in zip(frames, encoded):
        decoded = decompressor.decompress(compressed, max_output_size=len(source))
        exact &= decoded == source
    decode_seconds = time.monotonic() - begin
    return {
        "level": level,
        "frames": len(frames),
        "payload_bytes": payload,
        "wire_bytes": wire,
        "payload_ratio": payload / max(1, wire),
        "raw_input_ratio": raw_input / max(1, wire),
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "encode_payload_GBps": payload / max(encode_seconds, 1e-12) / 1e9,
        "decode_payload_GBps": payload / max(decode_seconds, 1e-12) / 1e9,
        "complete_effective_GBps": raw_input / max(encode_seconds + decode_seconds, 1e-12) / 1e9,
        "exact": bool(exact),
    }


def cdict_command(args: argparse.Namespace) -> int:
    result: dict = {
        "experiment": "leave_one_project_out_pretrained_zstd_dictionaries",
        "train": args.train,
        "test": args.test,
        "tracks": {},
    }
    begin_all = time.monotonic()
    for stream in args.streams:
        print(f"loading target stream {stream}", flush=True)
        frames, raw_input, payload = stream_frames(args.test, stream)
        samples, sample_stats = training_samples(args.train, stream, args.sample_count,
                                                 args.sample_bytes, args.seed)
        track: dict = {"sample_stats": sample_stats, "raw_input": raw_input,
                       "payload": payload, "rows": []}
        for level in args.levels:
            baseline = evaluate_dictionary(frames, raw_input, payload, None, level)
            baseline.update({"dictionary_bytes": 0, "package_track": "none"})
            track["rows"].append(baseline)
        for size_kib in args.sizes:
            requested = size_kib * 1024
            print(f"training {stream} dictionary {size_kib} KiB", flush=True)
            try:
                dictionary = zstd.train_dictionary(requested, samples)
            except zstd.ZstdError as exc:
                track["rows"].append({"dictionary_bytes": requested, "error": str(exc)})
                continue
            actual = len(dictionary.as_bytes())
            for level in args.levels:
                row = evaluate_dictionary(frames, raw_input, payload, dictionary, level)
                row.update({
                    "dictionary_bytes": actual,
                    "package_track": "GENERIC_II_BUILTIN",
                    "wire_model_total": row["wire_bytes"] + actual,
                    "wire_model_raw_input_ratio": raw_input / max(1, row["wire_bytes"] + actual),
                })
                track["rows"].append(row)
        result["tracks"][stream] = track
    result["wall_seconds"] = time.monotonic() - begin_all
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser("inspect")
    inspect.add_argument("dataset")
    inspect.set_defaults(func=inspect_command)

    ranker = sub.add_parser("ranker")
    ranker.add_argument("--train", nargs="+", required=True)
    ranker.add_argument("--test", required=True)
    ranker.add_argument("--samples", type=int, default=300_000)
    ranker.add_argument("--width", type=int, default=128)
    ranker.add_argument("--epochs", type=int, default=3)
    ranker.add_argument("--batch", type=int, default=1024)
    ranker.add_argument("--threads", type=int, default=12)
    ranker.add_argument("--seed", type=int, default=0x1CE50)
    ranker.add_argument("--online", action="store_true")
    ranker.add_argument("--online-lr", type=float, default=2e-4)
    ranker.add_argument("--model-out", required=True)
    ranker.add_argument("--ranks-out")
    ranker.add_argument("--online-ranks-out")
    ranker.add_argument("--report", required=True)
    ranker.set_defaults(func=ranker_command)

    neural = sub.add_parser("neural-suite")
    neural.add_argument("--train", nargs="+", required=True)
    neural.add_argument("--test", required=True)
    neural.add_argument("--samples", type=int, default=200_000)
    neural.add_argument("--test-events", type=int, default=30_000)
    neural.add_argument("--width", type=int, default=128)
    neural.add_argument("--epochs", type=int, default=3)
    neural.add_argument("--batch", type=int, default=512)
    neural.add_argument("--threads", type=int, default=12)
    neural.add_argument("--seed", type=int, default=0x1CE50)
    neural.add_argument("--model-out", required=True)
    neural.add_argument("--report", required=True)
    neural.set_defaults(func=neural_suite_command)

    gbdt = sub.add_parser("gbdt")
    gbdt.add_argument("--train", nargs="+", required=True)
    gbdt.add_argument("--test", required=True)
    gbdt.add_argument("--samples", type=int, default=500_000)
    gbdt.add_argument("--trees", type=int, default=256)
    gbdt.add_argument("--leaves", type=int, default=31)
    gbdt.add_argument("--depth", type=int, default=8)
    gbdt.add_argument("--learning-rate", type=float, default=0.05)
    gbdt.add_argument("--threads", type=int, default=12)
    gbdt.add_argument("--batch-rows", type=int, default=250_000)
    gbdt.add_argument("--seed", type=int, default=0x1CE50)
    gbdt.add_argument("--model-out", required=True)
    gbdt.add_argument("--report", required=True)
    gbdt.set_defaults(func=gbdt_command)

    cdict = sub.add_parser("cdict")
    cdict.add_argument("--train", nargs="+", required=True)
    cdict.add_argument("--test", required=True)
    cdict.add_argument("--streams", nargs="+", choices=("definition", "root-digest", "superblock"),
                       default=("definition", "root-digest", "superblock"))
    cdict.add_argument("--sizes", nargs="+", type=int, default=(32, 64, 128, 256))
    cdict.add_argument("--levels", nargs="+", type=int, default=(1, 3))
    cdict.add_argument("--sample-count", type=int, default=200_000)
    cdict.add_argument("--sample-bytes", type=int, default=256 << 20)
    cdict.add_argument("--seed", type=int, default=0x1CE50)
    cdict.add_argument("--report", required=True)
    cdict.set_defaults(func=cdict_command)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
