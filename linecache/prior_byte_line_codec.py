#!/usr/bin/env python3
"""Apply exact prior-frame byte copies to every P21 Line component."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
import zstandard as zstd

from generated_array_line_codec import (
    PART_NAMES,
    PreparedCorpus,
    decode_candidate,
    parse_trace,
    persistent_stream,
    prepare_corpus,
)
from line_stream_ceiling import load_corpus
from prior_byte_copy import PriorByteIndex, decode, encode


def persistent_prior_copy(
    prepared: PreparedCorpus,
    level: int,
    seed_length: int,
    candidates: int,
    stride: int,
) -> dict:
    stream_count = len(PART_NAMES)
    compressors = [
        zstd.ZstdCompressor(level=level, write_content_size=False).compressobj()
        for _ in range(stream_count)
    ]
    indexes = [
        PriorByteIndex(seed_length, candidates, stride)
        for _ in range(stream_count)
    ]
    encoded_frames: list[list[bytes | None]] = []
    expected_parts: list[list[bytes | None]] = []
    last_active: list[int | None] = [None] * stream_count
    component_copies = [0] * stream_count
    component_copied = [0] * stream_count

    encode_begin = time.monotonic()
    for frame_index, frame in enumerate(prepared.frames):
        encoded: list[bytes | None] = []
        expected_parts.append(list(frame.candidate))
        for index, (compressor, root_index, raw) in enumerate(
            zip(compressors, indexes, frame.candidate)
        ):
            if raw is None:
                encoded.append(None)
                continue
            program, copies, copied = encode(raw, root_index)
            encoded.append(
                compressor.compress(program)
                + compressor.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
            )
            component_copies[index] += copies
            component_copied[index] += copied
            root_index.add(raw)
            last_active[index] = frame_index
        encoded_frames.append(encoded)

    for index, compressor in enumerate(compressors):
        final_frame = last_active[index]
        if final_frame is not None:
            suffix = compressor.flush(zstd.COMPRESSOBJ_FLUSH_FINISH)
            current = encoded_frames[final_frame][index]
            if current is None:
                raise AssertionError("active prior-byte stream lost its final frame")
            encoded_frames[final_frame][index] = current + suffix
    encode_seconds = time.monotonic() - encode_begin

    decompressors = [
        zstd.ZstdDecompressor().decompressobj() for _ in range(stream_count)
    ]
    decoder_roots: list[list[bytes]] = [[] for _ in range(stream_count)]
    component_wire = [0] * stream_count
    decode_begin = time.monotonic()
    for frame, encoded, raw_parts in zip(
        prepared.frames, encoded_frames, expected_parts
    ):
        recovered_parts: list[bytes | None] = []
        for index, (decompressor, chunk, expected) in enumerate(
            zip(decompressors, encoded, raw_parts)
        ):
            if chunk is None:
                if expected is not None:
                    raise ValueError("prior-byte stream omitted an expected component")
                recovered_parts.append(None)
                continue
            program = decompressor.decompress(chunk)
            recovered = decode(program, decoder_roots[index])
            if recovered != expected:
                raise ValueError("prior-byte component differs after decode")
            decoder_roots[index].append(recovered)
            recovered_parts.append(recovered)
            component_wire[index] += len(chunk) + 4
        if decode_candidate(recovered_parts, prepared.extended) != frame.expected:
            raise ValueError("prior-byte reconstructed Lines differ from trace")
    decode_seconds = time.monotonic() - decode_begin
    for index, final_frame in enumerate(last_active):
        if final_frame is not None and not decompressors[index].eof:
            raise ValueError("prior-byte component stream did not finish")

    selector_wire = len(prepared.frames)
    return {
        "wire_bytes": sum(component_wire) + selector_wire,
        "selector_wire_bytes": selector_wire,
        "component_wire": dict(zip(PART_NAMES, component_wire)),
        "component_copies": dict(zip(PART_NAMES, component_copies)),
        "component_copied_bytes": dict(zip(PART_NAMES, component_copied)),
        "component_input_bytes": {
            name: index.root_bytes for name, index in zip(PART_NAMES, indexes)
        },
        "receiver_logical_bytes": sum(
            index.receiver_logical_bytes for index in indexes
        ),
        "encoder_index_logical_bytes": sum(
            index.index_logical_bytes for index in indexes
        ),
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "exact": True,
    }


def summarize(
    prepared: PreparedCorpus,
    level: int,
    seed_length: int,
    candidates: int,
    stride: int,
) -> dict:
    baseline = persistent_stream(prepared, level, True)
    candidate = persistent_prior_copy(
        prepared,
        level,
        seed_length,
        candidates,
        stride,
    )
    raw_bytes = sum(prepared.corpus.raw_by_tu)
    return {
        "corpus": prepared.corpus.name,
        "tus": len(prepared.corpus.raw_by_tu),
        "raw_bytes": raw_bytes,
        "line_bytes": prepared.corpus.line_bytes,
        "level": level,
        "seed_length": seed_length,
        "candidate_limit": candidates,
        "index_stride": stride,
        "baseline_wire_bytes": baseline["wire_bytes"],
        "prior_byte_wire_bytes": candidate["wire_bytes"],
        "saving_bytes": baseline["wire_bytes"] - candidate["wire_bytes"],
        "saving_fraction": 1 - candidate["wire_bytes"] / baseline["wire_bytes"],
        "baseline_component_wire": baseline["component_wire"],
        **candidate,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", action="append", type=parse_trace, required=True)
    parser.add_argument("--max-tus", type=int, default=0)
    parser.add_argument("--levels", nargs="+", type=int, choices=(1, 3), default=(3,))
    parser.add_argument("--seed-length", type=int, default=16)
    parser.add_argument("--candidates", type=int, default=4)
    parser.add_argument("--index-stride", type=int, default=8)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rows = []
    for name, path in args.trace:
        corpus = load_corpus(name, path, args.max_tus)
        prepared = prepare_corpus(corpus, extended=True)
        for level in args.levels:
            row = summarize(
                prepared,
                level,
                args.seed_length,
                args.candidates,
                args.index_stride,
            )
            rows.append(row)
            print(
                json.dumps(
                    {
                        "corpus": name,
                        "level": level,
                        "baseline": row["baseline_wire_bytes"],
                        "prior_byte": row["prior_byte_wire_bytes"],
                        "saving": row["saving_bytes"],
                        "exact": row["exact"],
                    },
                    sort_keys=True,
                ),
                flush=True,
            )
    report = {
        "experiment": "exact causal prior-frame byte copy over P21 components",
        "max_tus": args.max_tus,
        "seed_length": args.seed_length,
        "candidate_limit": args.candidates,
        "index_stride": args.index_stride,
        "rows": rows,
        "exact": all(row["exact"] for row in rows),
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
