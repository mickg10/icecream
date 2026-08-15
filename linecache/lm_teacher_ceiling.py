#!/usr/bin/env python3
"""Bounded pretrained code-LM ceiling on held-out definition residual bytes.

This row is deliberately not a codec result.  It measures residual predictability and runtime for
an externally pretrained teacher.  Its training set is not controlled against these repositories,
so the report labels it as an optimistic ceiling; it cannot qualify a deployment row.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import time
from pathlib import Path

import torch
import zstandard as zstd
from transformers import AutoModelForCausalLM, AutoTokenizer

from ml_models import Event, TuRecord, iter_dataset


def residual(event: Event) -> bytes:
    if not event.candidates:
        return event.line
    candidate = min(event.candidates, key=lambda value: value.cost)
    prefix = min(candidate.prefix, len(event.line))
    suffix = min(candidate.suffix, len(event.line) - prefix)
    return event.line[prefix:len(event.line) - suffix if suffix else len(event.line)]


def sample_residuals(path: str, byte_budget: int, line_capacity: int,
                     seed: int) -> tuple[bytes, dict]:
    rng = random.Random(seed)
    reservoir: list[bytes] = []
    seen = raw_bytes = residual_bytes = invalid_utf8 = 0
    for record in iter_dataset(path):
        if isinstance(record, TuRecord):
            raw_bytes += record.raw_bytes
        elif isinstance(record, Event):
            value = residual(record)
            residual_bytes += len(value)
            if not value:
                continue
            try:
                value.decode("utf-8")
            except UnicodeDecodeError:
                invalid_utf8 += len(value)
                continue
            seen += 1
            if len(reservoir) < line_capacity:
                reservoir.append(value)
            else:
                slot = rng.randrange(seen)
                if slot < line_capacity:
                    reservoir[slot] = value
    rng.shuffle(reservoir)
    output = bytearray()
    for value in reservoir:
        if len(output) + len(value) + 1 > byte_budget:
            remaining = byte_budget - len(output)
            if remaining > 1:
                output += value[:remaining - 1]
                output.append(10)
            break
        output += value
        output.append(10)
    return bytes(output), {
        "dataset_raw_bytes": raw_bytes,
        "all_residual_bytes": residual_bytes,
        "sampled_utf8_bytes": len(output),
        "invalid_utf8_residual_bytes_skipped": invalid_utf8,
        "eligible_lines_seen": seen,
        "reservoir_lines": len(reservoir),
    }


def run(args: argparse.Namespace) -> int:
    begin = time.monotonic()
    sample, sample_stats = sample_residuals(args.dataset, args.sample_bytes,
                                            args.sample_lines, args.seed)
    text = sample.decode("utf-8")
    tokenizer = AutoTokenizer.from_pretrained(args.model)
    tokens = tokenizer(text, return_tensors="pt", add_special_tokens=False).input_ids[0]
    dtype = {"float32": torch.float32, "bfloat16": torch.bfloat16}[args.dtype]
    model = AutoModelForCausalLM.from_pretrained(args.model, dtype=dtype,
                                                 low_cpu_mem_usage=True)
    model.eval()
    torch.set_num_threads(args.threads)
    total_nll = 0.0
    scored_tokens = 0
    inference_begin = time.monotonic()
    with torch.inference_mode():
        for start in range(0, max(0, len(tokens) - 1), args.context):
            chunk = tokens[start:start + args.context + 1]
            if len(chunk) < 2:
                continue
            logits = model(chunk[:-1][None, :]).logits[0]
            loss = torch.nn.functional.cross_entropy(logits, chunk[1:], reduction="sum")
            total_nll += float(loss)
            scored_tokens += len(chunk) - 1
    inference_seconds = time.monotonic() - inference_begin
    bits = total_nll / math.log(2)
    zrows = []
    for level in (1, 3, 6):
        compressor = zstd.ZstdCompressor(level=level)
        encoded = compressor.compress(sample)
        decoded = zstd.ZstdDecompressor().decompress(encoded)
        zrows.append({"level": level, "bytes": len(encoded),
                      "bits_per_input_byte": 8 * len(encoded) / max(1, len(sample)),
                      "exact": decoded == sample})
    result = {
        "experiment": "external_pretrained_code_lm_residual_ceiling",
        "status": "optimistic_teacher_only_uncontrolled_training_overlap",
        "model": args.model,
        "dataset": args.dataset,
        "sample": sample_stats,
        "tokens": int(len(tokens)),
        "scored_tokens": scored_tokens,
        "negative_log_likelihood_bits": bits,
        "bits_per_sample_byte": bits / max(1, len(sample)),
        "bits_per_token": bits / max(1, scored_tokens),
        "perplexity": math.exp(total_nll / max(1, scored_tokens)),
        "parameters": sum(parameter.numel() for parameter in model.parameters()),
        "resident_parameter_bytes": sum(parameter.numel() * parameter.element_size()
                                        for parameter in model.parameters()),
        "dtype": args.dtype,
        "inference_seconds": inference_seconds,
        "tokens_per_second": scored_tokens / max(1e-12, inference_seconds),
        "sample_bytes_per_second": len(sample) / max(1e-12, inference_seconds),
        "zstd_controls": zrows,
        "exact_bitstream": False,
        "wall_seconds": time.monotonic() - begin,
    }
    Path(args.report).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--model", default="Qwen/Qwen2.5-Coder-0.5B")
    value.add_argument("--dataset", required=True)
    value.add_argument("--sample-bytes", type=int, default=64 << 10)
    value.add_argument("--sample-lines", type=int, default=100_000)
    value.add_argument("--context", type=int, default=512)
    value.add_argument("--dtype", choices=("float32", "bfloat16"), default="bfloat16")
    value.add_argument("--threads", type=int, default=12)
    value.add_argument("--seed", type=int, default=0x7EAC4E)
    value.add_argument("--report", required=True)
    return value


if __name__ == "__main__":
    raise SystemExit(run(parser().parse_args()))
