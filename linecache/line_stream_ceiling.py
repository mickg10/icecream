#!/usr/bin/env python3
"""Measure exact Line-text framing, persistent-stream, and whole-corpus ceilings."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import os
import struct
import time
from pathlib import Path
from typing import BinaryIO, Callable, Sequence

import zstandard as zstd


U8 = struct.Struct("<B")
U32 = struct.Struct("<I")
TU_HEAD = struct.Struct("<IQI")
FOOTER = struct.Struct("<QQQQQ")
REGION_BYTES = 28
EVENT_FIXED = struct.Struct("<IIIIIIIIQQQQ")
CANDIDATE_BYTES = 40


@dataclasses.dataclass(slots=True)
class Corpus:
    name: str
    path: str
    raw_by_tu: list[int]
    lines_by_tu: list[list[bytes]]
    line_bytes: int
    candidates: int


def read_exact(source: BinaryIO, size: int) -> bytes:
    value = source.read(size)
    if len(value) != size:
        raise EOFError(f"wanted {size} bytes, got {len(value)}")
    return value


def put_varint(value: int) -> bytes:
    output = bytearray()
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)
    return bytes(output)


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
    raise ValueError("incomplete varint")


def load_corpus(name: str, path: str, max_tus: int) -> Corpus:
    raw_by_tu: list[int] = []
    lines_by_tu: list[list[bytes]] = []
    line_bytes = events = candidates = 0
    with open(path, "rb") as source:
        if read_exact(source, 8) != b"ICMLDS2\0":
            raise ValueError(f"{path}: wrong trace magic")
        if U32.unpack(read_exact(source, U32.size))[0] != 2:
            raise ValueError(f"{path}: unsupported trace version")
        while True:
            tag = U8.unpack(read_exact(source, U8.size))[0]
            if tag == 0:
                footer = FOOTER.unpack(read_exact(source, FOOTER.size))
                if source.read(1):
                    raise ValueError(f"{path}: trailing bytes")
                if not max_tus and footer != (
                    sum(raw_by_tu),
                    line_bytes,
                    len(raw_by_tu),
                    events,
                    candidates,
                ):
                    raise ValueError(f"{path}: footer differs from observed records")
                break
            if tag == 1:
                tu, raw_bytes, regions = TU_HEAD.unpack(
                    read_exact(source, TU_HEAD.size)
                )
                if tu != len(raw_by_tu):
                    raise ValueError(f"{path}: non-sequential TU")
                if max_tus and tu >= max_tus:
                    return Corpus(
                        name,
                        path,
                        raw_by_tu,
                        lines_by_tu,
                        line_bytes,
                        candidates,
                    )
                raw_by_tu.append(raw_bytes)
                lines_by_tu.append([])
                source.seek(regions * REGION_BYTES, os.SEEK_CUR)
                continue
            if tag != 2:
                raise ValueError(f"{path}: unknown record tag {tag}")
            fixed = EVENT_FIXED.unpack(read_exact(source, EVENT_FIXED.size))
            tu = fixed[0]
            if tu >= len(lines_by_tu):
                raise ValueError(f"{path}: Line record precedes its TU")
            line_size = U32.unpack(read_exact(source, U32.size))[0]
            line = read_exact(source, line_size)
            count = U32.unpack(read_exact(source, U32.size))[0]
            source.seek(count * CANDIDATE_BYTES, os.SEEK_CUR)
            lines_by_tu[tu].append(line)
            line_bytes += line_size
            events += 1
            candidates += count
    return Corpus(name, path, raw_by_tu, lines_by_tu, line_bytes, candidates)


def serialize_literal(lines: Sequence[bytes]) -> bytes:
    return put_varint(len(lines)) + b"".join(
        put_varint(len(line)) + line for line in lines
    )


def serialize_front(lines: Sequence[bytes]) -> bytes:
    ordered = sorted(lines)
    output = bytearray(put_varint(len(ordered)))
    previous = b""
    for line in ordered:
        limit = min(len(previous), len(line))
        lcp = 0
        while lcp < limit and previous[lcp] == line[lcp]:
            lcp += 1
        output += put_varint(lcp)
        output += put_varint(len(line) - lcp)
        output += line[lcp:]
        previous = line
    return bytes(output)


def serialize_front_split(lines: Sequence[bytes]) -> tuple[bytes, bytes, bytes]:
    ordered = sorted(lines)
    lcp_stream = bytearray(put_varint(len(ordered)))
    length_stream = bytearray()
    suffix_stream = bytearray()
    previous = b""
    for line in ordered:
        limit = min(len(previous), len(line))
        lcp = 0
        while lcp < limit and previous[lcp] == line[lcp]:
            lcp += 1
        suffix = line[lcp:]
        lcp_stream += put_varint(lcp)
        length_stream += put_varint(len(suffix))
        suffix_stream += suffix
        previous = line
    return bytes(lcp_stream), bytes(length_stream), bytes(suffix_stream)


def decode_literal(raw: bytes) -> list[bytes]:
    count, offset = get_varint(raw, 0)
    lines: list[bytes] = []
    for _ in range(count):
        size, offset = get_varint(raw, offset)
        end = offset + size
        if end > len(raw):
            raise ValueError("literal Line exceeds frame")
        lines.append(raw[offset:end])
        offset = end
    if offset != len(raw):
        raise ValueError("literal frame has trailing bytes")
    return lines


def decode_front(raw: bytes) -> list[bytes]:
    count, offset = get_varint(raw, 0)
    lines: list[bytes] = []
    previous = b""
    for _ in range(count):
        lcp, offset = get_varint(raw, offset)
        suffix, offset = get_varint(raw, offset)
        end = offset + suffix
        if lcp > len(previous) or end > len(raw):
            raise ValueError("front-coded Line exceeds frame")
        line = previous[:lcp] + raw[offset:end]
        lines.append(line)
        previous = line
        offset = end
    if offset != len(raw):
        raise ValueError("front-coded frame has trailing bytes")
    return lines


def decode_front_split(parts: Sequence[bytes]) -> list[bytes]:
    lcp_stream, length_stream, suffix_stream = parts
    count, lcp_offset = get_varint(lcp_stream, 0)
    length_offset = suffix_offset = 0
    lines: list[bytes] = []
    previous = b""
    for _ in range(count):
        lcp, lcp_offset = get_varint(lcp_stream, lcp_offset)
        suffix_size, length_offset = get_varint(length_stream, length_offset)
        suffix_end = suffix_offset + suffix_size
        if lcp > len(previous) or suffix_end > len(suffix_stream):
            raise ValueError("split front-coded Line exceeds its stream")
        line = previous[:lcp] + suffix_stream[suffix_offset:suffix_end]
        lines.append(line)
        previous = line
        suffix_offset = suffix_end
    if (
        lcp_offset != len(lcp_stream)
        or length_offset != len(length_stream)
        or suffix_offset != len(suffix_stream)
    ):
        raise ValueError("split front-coded stream has trailing bytes")
    return lines


def independent_best(
    lines_by_tu: Sequence[Sequence[bytes]], level: int
) -> dict[str, int | float | bool]:
    compressor = zstd.ZstdCompressor(level=level)
    decompressor = zstd.ZstdDecompressor()
    wire = encode_seconds = decode_seconds = 0
    literal_wins = front_wins = 0
    for lines in lines_by_tu:
        if not lines:
            continue
        literal_raw = serialize_literal(lines)
        front_raw = serialize_front(lines)
        begin = time.monotonic()
        literal = compressor.compress(literal_raw)
        front = compressor.compress(front_raw)
        encode_seconds += time.monotonic() - begin
        if len(front) < len(literal):
            raw, encoded, decoder, expected = (
                front_raw,
                front,
                decode_front,
                sorted(lines),
            )
            front_wins += 1
        else:
            raw, encoded, decoder, expected = (
                literal_raw,
                literal,
                decode_literal,
                list(lines),
            )
            literal_wins += 1
        begin = time.monotonic()
        recovered = decompressor.decompress(encoded)
        decoded = decoder(recovered)
        decode_seconds += time.monotonic() - begin
        if recovered != raw or decoded != expected:
            raise ValueError("independent Line frame differs after decode")
        wire += 4 + 1 + len(encoded)
    return {
        "wire_bytes": wire,
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "literal_wins": literal_wins,
        "front_wins": front_wins,
        "exact": True,
    }


def stream_mode(
    lines_by_tu: Sequence[Sequence[bytes]],
    level: int,
    serializer: Callable[[Sequence[bytes]], bytes],
    decoder: Callable[[bytes], list[bytes]],
    ordered: bool,
) -> dict[str, int | float | bool]:
    frames = [(list(lines), serializer(lines)) for lines in lines_by_tu if lines]
    compressor = zstd.ZstdCompressor(
        level=level,
        write_content_size=False,
    ).compressobj()
    chunks: list[bytes] = []
    encode_begin = time.monotonic()
    for _, raw in frames:
        chunks.append(
            compressor.compress(raw)
            + compressor.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        )
    if chunks:
        chunks[-1] += compressor.flush(zstd.COMPRESSOBJ_FLUSH_FINISH)
    encode_seconds = time.monotonic() - encode_begin

    decompressor = zstd.ZstdDecompressor().decompressobj()
    decode_begin = time.monotonic()
    for (lines, raw), chunk in zip(frames, chunks):
        recovered = decompressor.decompress(chunk)
        expected = sorted(lines) if ordered else lines
        if recovered != raw or decoder(recovered) != expected:
            raise ValueError("streamed Line block differs after decode")
    decode_seconds = time.monotonic() - decode_begin
    if chunks and not decompressor.eof:
        raise ValueError("streamed Line frame did not finish")
    return {
        "wire_bytes": sum(len(chunk) + 4 for chunk in chunks),
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "literal_wins": len(chunks) if not ordered else 0,
        "front_wins": len(chunks) if ordered else 0,
        "exact": True,
    }


def independent_split_front(
    lines_by_tu: Sequence[Sequence[bytes]], level: int
) -> dict[str, int | float | bool]:
    compressor = zstd.ZstdCompressor(level=level)
    decompressor = zstd.ZstdDecompressor()
    wire = 0
    encode_seconds = decode_seconds = 0.0
    frames = 0
    for lines in lines_by_tu:
        if not lines:
            continue
        raw_parts = serialize_front_split(lines)
        begin = time.monotonic()
        encoded_parts = [compressor.compress(raw) for raw in raw_parts]
        encode_seconds += time.monotonic() - begin
        begin = time.monotonic()
        recovered_parts = [
            decompressor.decompress(encoded) for encoded in encoded_parts
        ]
        decoded = decode_front_split(recovered_parts)
        decode_seconds += time.monotonic() - begin
        if tuple(recovered_parts) != raw_parts or decoded != sorted(lines):
            raise ValueError("independent split front frame differs after decode")
        wire += sum(len(encoded) + 4 for encoded in encoded_parts)
        frames += 1
    return {
        "wire_bytes": wire,
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "literal_wins": 0,
        "front_wins": frames,
        "exact": True,
    }


def stream_split_front(
    lines_by_tu: Sequence[Sequence[bytes]], level: int
) -> dict[str, int | float | bool]:
    frames = [
        (list(lines), serialize_front_split(lines))
        for lines in lines_by_tu
        if lines
    ]
    compressors = [
        zstd.ZstdCompressor(level=level, write_content_size=False).compressobj()
        for _ in range(3)
    ]
    chunks: list[list[bytes]] = []
    encode_begin = time.monotonic()
    for _, raw_parts in frames:
        chunks.append(
            [
                compressor.compress(raw)
                + compressor.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
                for compressor, raw in zip(compressors, raw_parts)
            ]
        )
    if chunks:
        for index, compressor in enumerate(compressors):
            chunks[-1][index] += compressor.flush(
                zstd.COMPRESSOBJ_FLUSH_FINISH
            )
    encode_seconds = time.monotonic() - encode_begin

    decompressors = [zstd.ZstdDecompressor().decompressobj() for _ in range(3)]
    decode_begin = time.monotonic()
    for (lines, raw_parts), encoded_parts in zip(frames, chunks):
        recovered_parts = [
            decompressor.decompress(encoded)
            for decompressor, encoded in zip(decompressors, encoded_parts)
        ]
        if tuple(recovered_parts) != raw_parts:
            raise ValueError("streamed split front bytes differ after decode")
        if decode_front_split(recovered_parts) != sorted(lines):
            raise ValueError("streamed split front Lines differ after decode")
    decode_seconds = time.monotonic() - decode_begin
    if chunks and not all(decompressor.eof for decompressor in decompressors):
        raise ValueError("split front streams did not finish")
    return {
        "wire_bytes": sum(
            len(encoded) + 4 for parts in chunks for encoded in parts
        ),
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "literal_wins": 0,
        "front_wins": len(chunks),
        "exact": True,
    }


def whole_mode(
    lines: Sequence[bytes],
    level: int,
    serializer: Callable[[Sequence[bytes]], bytes],
    decoder: Callable[[bytes], list[bytes]],
    ordered: bool,
) -> dict[str, int | float | bool]:
    raw = serializer(lines)
    compressor = zstd.ZstdCompressor(level=level)
    begin = time.monotonic()
    encoded = compressor.compress(raw)
    encode_seconds = time.monotonic() - begin
    begin = time.monotonic()
    recovered = zstd.ZstdDecompressor().decompress(encoded)
    decoded = decoder(recovered)
    decode_seconds = time.monotonic() - begin
    expected = sorted(lines) if ordered else list(lines)
    if recovered != raw or decoded != expected:
        raise ValueError("whole Line frame differs after decode")
    return {
        "wire_bytes": len(encoded) + 4,
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "literal_wins": 1 if not ordered else 0,
        "front_wins": 1 if ordered else 0,
        "exact": True,
    }


def whole_split_front(
    lines: Sequence[bytes], level: int
) -> dict[str, int | float | bool]:
    raw_parts = serialize_front_split(lines)
    compressor = zstd.ZstdCompressor(level=level)
    begin = time.monotonic()
    encoded_parts = [compressor.compress(raw) for raw in raw_parts]
    encode_seconds = time.monotonic() - begin
    decompressor = zstd.ZstdDecompressor()
    begin = time.monotonic()
    recovered_parts = [
        decompressor.decompress(encoded) for encoded in encoded_parts
    ]
    decoded = decode_front_split(recovered_parts)
    decode_seconds = time.monotonic() - begin
    if tuple(recovered_parts) != raw_parts or decoded != sorted(lines):
        raise ValueError("whole split front frame differs after decode")
    return {
        "wire_bytes": sum(len(encoded) + 4 for encoded in encoded_parts),
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "literal_wins": 0,
        "front_wins": 1,
        "exact": True,
    }


def evaluate(corpus: Corpus, level: int) -> list[dict]:
    all_lines = [line for lines in corpus.lines_by_tu for line in lines]
    modes = (
        ("independent-best", independent_best(corpus.lines_by_tu, level)),
        (
            "independent-split-front",
            independent_split_front(corpus.lines_by_tu, level),
        ),
        (
            "stream-literal",
            stream_mode(
                corpus.lines_by_tu,
                level,
                serialize_literal,
                decode_literal,
                False,
            ),
        ),
        (
            "stream-front",
            stream_mode(
                corpus.lines_by_tu,
                level,
                serialize_front,
                decode_front,
                True,
            ),
        ),
        ("stream-split-front", stream_split_front(corpus.lines_by_tu, level)),
        (
            "whole-literal",
            whole_mode(all_lines, level, serialize_literal, decode_literal, False),
        ),
        (
            "whole-front",
            whole_mode(all_lines, level, serialize_front, decode_front, True),
        ),
        ("whole-split-front", whole_split_front(all_lines, level)),
    )
    rows: list[dict] = []
    for mode, measurement in modes:
        wire = int(measurement["wire_bytes"])
        encode_seconds = float(measurement["encode_seconds"])
        decode_seconds = float(measurement["decode_seconds"])
        rows.append(
            {
                "corpus": corpus.name,
                "level": level,
                "mode": mode,
                "tus": len(corpus.raw_by_tu),
                "raw_bytes": sum(corpus.raw_by_tu),
                "line_bytes": corpus.line_bytes,
                "lines": len(all_lines),
                "wire_bytes": wire,
                "line_ratio": corpus.line_bytes / max(1, wire),
                "raw_ratio": sum(corpus.raw_by_tu) / max(1, wire),
                "encode_seconds": encode_seconds,
                "decode_seconds": decode_seconds,
                "encode_line_GBps": corpus.line_bytes
                / max(encode_seconds, 1e-12)
                / 1e9,
                "decode_line_GBps": corpus.line_bytes
                / max(decode_seconds, 1e-12)
                / 1e9,
                "literal_wins": measurement["literal_wins"],
                "front_wins": measurement["front_wins"],
                "exact": measurement["exact"],
            }
        )
    return rows


def parse_trace(value: str) -> tuple[str, str]:
    name, separator, path = value.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError("trace must be NAME=PATH")
    return name, path


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--trace", action="append", type=parse_trace, required=True)
    value.add_argument("--levels", nargs="+", type=int, default=(1, 3, 6))
    value.add_argument("--max-tus", type=int, default=0)
    value.add_argument("--output", required=True)
    value.add_argument("--tsv")
    return value


def main() -> int:
    args = parser().parse_args()
    rows: list[dict] = []
    for name, path in args.trace:
        begin = time.monotonic()
        corpus = load_corpus(name, path, args.max_tus)
        print(
            f"LOAD {name} tus={len(corpus.raw_by_tu)} "
            f"lines={sum(map(len, corpus.lines_by_tu))} "
            f"bytes={corpus.line_bytes} seconds={time.monotonic() - begin:.3f}",
            flush=True,
        )
        for level in args.levels:
            for row in evaluate(corpus, level):
                rows.append(row)
                print(
                    f"PASS {name} z{level} {row['mode']} "
                    f"wire={row['wire_bytes']} line_ratio={row['line_ratio']:.3f} "
                    f"raw_ratio={row['raw_ratio']:.3f}",
                    flush=True,
                )
    report = {
        "experiment": "exact Line-text stream and whole-corpus ceilings",
        "levels": args.levels,
        "max_tus": args.max_tus,
        "rows": rows,
    }
    Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.tsv:
        with open(args.tsv, "w", newline="") as output:
            writer = csv.DictWriter(
                output,
                fieldnames=tuple(rows[0]),
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
