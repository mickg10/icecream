#!/usr/bin/env python3
"""Compile immutable integer table clusters for exact superblock payload coding."""

from __future__ import annotations

import argparse
import json
import struct
import time
from pathlib import Path
from typing import Iterator, Sequence

import numpy as np
from sklearn.cluster import MiniBatchKMeans

from pretrained_superblocks import (ExactDecoder, ExactEncoder, iter_tus_fast,
                                     package_from_keys, superblock_keys)


FRAME_MAGIC = b"ICFRM1\0\0"
MODEL_MAGIC = b"ICTBL1\0\0"
U32 = struct.Struct("<I")
FRAME_HEADER = struct.Struct("<QI")
MODEL_HEADER = struct.Struct("<IIII")
CONTEXTS = 4096
TOTAL = 4096


def iter_frames(path: str) -> Iterator[tuple[int, bytes]]:
    with open(path, "rb") as f:
        if f.read(8) != FRAME_MAGIC:
            raise ValueError(f"{path}: bad frame magic")
        if U32.unpack(f.read(4))[0] != 1:
            raise ValueError(f"{path}: bad frame version")
        count = U32.unpack(f.read(4))[0]
        for _ in range(count):
            raw, size = FRAME_HEADER.unpack(f.read(FRAME_HEADER.size))
            payload = f.read(size)
            if len(payload) != size:
                raise EOFError(path)
            yield raw, payload
        if f.read(1):
            raise ValueError(f"{path}: trailing data")


def prepare_command(args: argparse.Namespace) -> int:
    begin = time.monotonic()
    count = raw_bytes = payload_bytes = 0
    with open(args.output, "wb") as output:
        output.write(FRAME_MAGIC)
        output.write(U32.pack(1))
        output.write(U32.pack(0))
        for dataset in args.datasets:
            package = package_from_keys([], 0)
            encoder = ExactEncoder(package, {})
            decoder = ExactDecoder(package, {})
            for tu in iter_tus_fast(dataset):
                keys = superblock_keys(tu)
                payload = encoder.frame(keys)
                recovered = decoder.frame(payload)
                expected = [(a, b, n) for a, b, _, n in tu.regions]
                if recovered != expected:
                    raise ValueError(f"{dataset}: exact frame preparation failed TU {tu.tu}")
                output.write(FRAME_HEADER.pack(tu.raw_bytes, len(payload)))
                output.write(payload)
                count += 1
                raw_bytes += tu.raw_bytes
                payload_bytes += len(payload)
        output.seek(12)
        output.write(U32.pack(count))
    result = {
        "operation": "prepare_exact_dynamic_superblock_frames",
        "datasets": args.datasets,
        "output": args.output,
        "frames": count,
        "raw_bytes": raw_bytes,
        "payload_bytes": payload_bytes,
        "seconds": time.monotonic() - begin,
        "exact": True,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def context_counts(paths: Sequence[str]) -> tuple[np.ndarray, dict]:
    counts = np.zeros((CONTEXTS, 256), dtype=np.uint64)
    frames = raw_bytes = payload_bytes = 0
    begin = time.monotonic()
    for path in paths:
        for raw, payload in iter_frames(path):
            value = np.frombuffer(payload, dtype=np.uint8)
            if len(value):
                previous = np.empty_like(value)
                previous2 = np.empty_like(value)
                previous[0] = 0
                previous[1:] = value[:-1]
                previous2[:min(2, len(value))] = 0
                if len(value) > 2:
                    previous2[2:] = value[:-2]
                contexts = ((previous2.astype(np.uint16) * 257 + previous) &
                            (CONTEXTS - 1)).astype(np.intp)
                np.add.at(counts, (contexts, value), 1)
            frames += 1
            raw_bytes += raw
            payload_bytes += len(payload)
    return counts, {
        "frames": frames, "raw_bytes": raw_bytes, "payload_bytes": payload_bytes,
        "seconds": time.monotonic() - begin,
    }


def quantize(counts: np.ndarray, total: int) -> np.ndarray:
    probability = (counts.astype(np.float64) + 0.25)
    probability /= probability.sum()
    remaining = total - 256
    exact = probability * remaining
    frequency = np.floor(exact).astype(np.int64) + 1
    difference = total - int(frequency.sum())
    fraction = exact - np.floor(exact)
    if difference > 0:
        order = np.argsort(-fraction)
        frequency[order[:difference]] += 1
    elif difference < 0:
        order = np.argsort(fraction)
        needed = -difference
        for index in order:
            removable = min(needed, int(frequency[index] - 1))
            frequency[index] -= removable
            needed -= removable
            if not needed:
                break
        if needed:
            raise ValueError("cannot normalize frequencies")
    if int(frequency.sum()) != total or np.any(frequency <= 0):
        raise ValueError("bad quantized frequency table")
    return frequency.astype(np.uint16)


def train_command(args: argparse.Namespace) -> int:
    begin = time.monotonic()
    counts, stats = context_counts(args.train_frames)
    weights = counts.sum(axis=1).astype(np.float64)
    probability = (counts.astype(np.float64) + args.smoothing)
    probability /= probability.sum(axis=1, keepdims=True)
    # Hellinger geometry makes the clustering objective track coding regret better than raw L2.
    features = np.sqrt(probability)
    result = {
        "experiment": "pretrained_context_mixture_integer_table_clusters",
        "train_frames": args.train_frames,
        "training": stats,
        "rows": [],
    }
    for clusters in args.clusters:
        model = MiniBatchKMeans(n_clusters=clusters, batch_size=CONTEXTS,
                                n_init=5, max_iter=args.iterations,
                                random_state=args.seed + clusters, reassignment_ratio=0.0)
        model.fit(features, sample_weight=np.maximum(weights, 1.0))
        mapping = model.labels_.astype(np.uint16)
        tables = np.zeros((clusters, 256), dtype=np.uint64)
        for context in range(CONTEXTS):
            tables[mapping[context]] += counts[context]
        frequencies = np.stack([quantize(table, TOTAL) for table in tables])
        path = f"{args.model_prefix}-{clusters}.bin"
        with open(path, "wb") as output:
            output.write(MODEL_MAGIC)
            output.write(MODEL_HEADER.pack(1, CONTEXTS, clusters, TOTAL))
            output.write(mapping.astype("<u2", copy=False).tobytes())
            output.write(frequencies.astype("<u2", copy=False).tobytes())
        # Cross-entropy on training bytes is a diagnostic; table_codec_bench reports real bytes.
        selected = frequencies[mapping].astype(np.float64) / TOTAL
        bits = float(-(counts * np.log2(selected)).sum())
        symbols = int(counts.sum())
        row = {
            "clusters": clusters,
            "model": path,
            "model_bytes": Path(path).stat().st_size,
            "training_bits_per_byte": bits / max(1, symbols),
            "contexts_used": int(np.count_nonzero(weights)),
            "exact_integer_total": TOTAL,
        }
        result["rows"].append(row)
        print(json.dumps({"row": row}), flush=True)
    result["wall_seconds"] = time.monotonic() - begin
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser()
    sub = root.add_subparsers(dest="command", required=True)
    prepare = sub.add_parser("prepare")
    prepare.add_argument("--datasets", nargs="+", required=True)
    prepare.add_argument("--output", required=True)
    prepare.set_defaults(func=prepare_command)
    train = sub.add_parser("train")
    train.add_argument("--train-frames", nargs="+", required=True)
    train.add_argument("--clusters", nargs="+", type=int, default=(64, 128, 256))
    train.add_argument("--iterations", type=int, default=200)
    train.add_argument("--smoothing", type=float, default=0.25)
    train.add_argument("--seed", type=int, default=0x7AB1E)
    train.add_argument("--model-prefix", required=True)
    train.add_argument("--report", required=True)
    train.set_defaults(func=train_command)
    return root


if __name__ == "__main__":
    arguments = parser().parse_args()
    raise SystemExit(arguments.func(arguments))
