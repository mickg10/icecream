#!/usr/bin/env python3
"""Exact generated-byte-array Line codec for issue #16.

The input is the authoritative ICMLDS2 chronological first-Line trace.  A strict parser recognizes
only canonical comma-separated decimal rows whose values fit in one byte.  The candidate sends the
underlying u8 values plus exact whitespace style and values-per-line control, while every other Line
stays in the existing lexicographic split-front representation.

Every reported frame is decompressed by an independent path and every reconstructed Line is compared
with the trace.  The script reports both a product-shaped independent per-TU best-of row and a
persistent zstd-stream ceiling comparable with line_stream_ceiling.py's stream-split-front row.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import heapq
import json
import time
from pathlib import Path
from typing import Sequence

import zstandard as zstd

from line_stream_ceiling import (
    Corpus,
    decode_front_split,
    get_varint,
    load_corpus,
    parse_trace,
    put_varint,
)


PART_NAMES = ("rest_lcp", "rest_length", "rest_suffix", "array_control", "array_values")


@dataclasses.dataclass(frozen=True, slots=True)
class ByteArrayLine:
    values: bytes
    prefix: bytes
    separator: bytes


@dataclasses.dataclass(slots=True)
class PreparedFrame:
    expected: list[bytes]
    baseline: tuple[bytes, bytes, bytes]
    candidate: tuple[bytes | None, bytes | None, bytes | None, bytes | None, bytes | None]
    array_lines: int
    array_text_bytes: int
    array_values: int
    styles: int


@dataclasses.dataclass(slots=True)
class PreparedCorpus:
    corpus: Corpus
    frames: list[PreparedFrame]
    prepare_seconds: float


def parse_decimal_byte_line(line: bytes) -> ByteArrayLine | None:
    """Return a lossless generated-array parse, or None for the literal fallback.

    Accepted rows have this exact grammar:

        horizontal-whitespace (canonical-decimal-byte comma separator)* newline

    There must be at least four values, every value has canonical decimal spelling, every value has
    a trailing comma, and the same horizontal separator occurs between all values.  A final comma is
    immediately followed by LF.  The narrow grammar makes false acceptance harmless and unlikely;
    the independent renderer remains the final exactness check.
    """

    if len(line) < 10 or not line.endswith(b",\n"):
        return None
    end = len(line) - 1
    position = 0
    while position < end and line[position] in (0x20, 0x09):
        position += 1
    prefix = line[:position]
    separator: bytes | None = None
    values = bytearray()

    while position < end:
        begin = position
        while position < end and 0x30 <= line[position] <= 0x39:
            position += 1
        if begin == position:
            return None
        digits = line[begin:position]
        if len(digits) > 1 and digits[0] == 0x30:
            return None
        value = 0
        for digit in digits:
            value = value * 10 + digit - 0x30
        if value > 255 or position >= end or line[position] != 0x2C:
            return None
        values.append(value)
        position += 1
        if position == end:
            break

        whitespace_begin = position
        while position < end and line[position] in (0x20, 0x09):
            position += 1
        current = line[whitespace_begin:position]
        if separator is None:
            separator = current
        elif current != separator:
            return None

    if position != end or len(values) < 4:
        return None
    return ByteArrayLine(bytes(values), prefix, separator or b"")


def render_decimal_byte_line(value: ByteArrayLine) -> bytes:
    output = bytearray(value.prefix)
    for index, byte in enumerate(value.values):
        output.extend(str(byte).encode("ascii"))
        output.append(0x2C)
        if index + 1 != len(value.values):
            output.extend(value.separator)
    output.append(0x0A)
    return bytes(output)


def serialize_front_split_sorted(lines: Sequence[bytes]) -> tuple[bytes, bytes, bytes]:
    lcp_stream = bytearray(put_varint(len(lines)))
    length_stream = bytearray()
    suffix_stream = bytearray()
    previous = b""
    for line in lines:
        limit = min(len(previous), len(line))
        lcp = 0
        while lcp < limit and previous[lcp] == line[lcp]:
            lcp += 1
        suffix = line[lcp:]
        lcp_stream.extend(put_varint(lcp))
        length_stream.extend(put_varint(len(suffix)))
        suffix_stream.extend(suffix)
        previous = line
    return bytes(lcp_stream), bytes(length_stream), bytes(suffix_stream)


def serialize_array_candidate(
    ordered: Sequence[bytes],
) -> tuple[
    tuple[bytes | None, bytes | None, bytes | None, bytes | None, bytes | None],
    dict[str, int],
]:
    rest: list[bytes] = []
    arrays: list[tuple[bytes, ByteArrayLine]] = []
    for line in ordered:
        parsed = parse_decimal_byte_line(line)
        if parsed is None:
            rest.append(line)
        else:
            arrays.append((line, parsed))

    rest_parts: tuple[bytes | None, bytes | None, bytes | None]
    if rest:
        rest_parts = serialize_front_split_sorted(rest)
    else:
        rest_parts = (None, None, None)

    if not arrays:
        return (*rest_parts, None, None), {
            "array_lines": 0,
            "array_text_bytes": 0,
            "array_values": 0,
            "styles": 0,
        }

    styles = sorted({(parsed.prefix, parsed.separator) for _, parsed in arrays})
    style_ids = {style: ordinal for ordinal, style in enumerate(styles)}
    control = bytearray()
    control.extend(put_varint(len(styles)))
    for prefix, separator in styles:
        control.extend(put_varint(len(prefix)))
        control.extend(prefix)
        control.extend(put_varint(len(separator)))
        control.extend(separator)
    control.extend(put_varint(len(arrays)))

    values = bytearray()
    for _, parsed in arrays:
        control.extend(put_varint(style_ids[(parsed.prefix, parsed.separator)]))
        control.extend(put_varint(len(parsed.values)))
        values.extend(parsed.values)

    return (*rest_parts, bytes(control), bytes(values)), {
        "array_lines": len(arrays),
        "array_text_bytes": sum(len(line) for line, _ in arrays),
        "array_values": len(values),
        "styles": len(styles),
    }


def decode_array_streams(control: bytes, values: bytes) -> list[bytes]:
    style_count, position = get_varint(control, 0)
    styles: list[tuple[bytes, bytes]] = []
    for _ in range(style_count):
        prefix_size, position = get_varint(control, position)
        prefix_end = position + prefix_size
        if prefix_end > len(control):
            raise ValueError("array style prefix exceeds control")
        prefix = control[position:prefix_end]
        position = prefix_end
        separator_size, position = get_varint(control, position)
        separator_end = position + separator_size
        if separator_end > len(control):
            raise ValueError("array style separator exceeds control")
        separator = control[position:separator_end]
        position = separator_end
        styles.append((prefix, separator))

    line_count, position = get_varint(control, position)
    output: list[bytes] = []
    value_position = 0
    for _ in range(line_count):
        style_id, position = get_varint(control, position)
        count, position = get_varint(control, position)
        if style_id >= len(styles) or count > len(values) - value_position:
            raise ValueError("array record exceeds its style or value stream")
        prefix, separator = styles[style_id]
        parsed = ByteArrayLine(
            values[value_position:value_position + count], prefix, separator
        )
        output.append(render_decimal_byte_line(parsed))
        value_position += count
    if position != len(control) or value_position != len(values):
        raise ValueError("array streams have trailing bytes")
    return output


def decode_candidate(
    parts: Sequence[bytes | None],
) -> list[bytes]:
    if len(parts) != len(PART_NAMES):
        raise ValueError("wrong generated-array part count")
    rest_presence = tuple(part is not None for part in parts[:3])
    if rest_presence == (True, True, True):
        rest = decode_front_split([part or b"" for part in parts[:3]])
    elif rest_presence == (False, False, False):
        rest = []
    else:
        raise ValueError("partial split-front record")

    if parts[3] is None and parts[4] is None:
        arrays: list[bytes] = []
    elif parts[3] is not None and parts[4] is not None:
        arrays = decode_array_streams(parts[3], parts[4])
    else:
        raise ValueError("partial generated-array record")
    return list(heapq.merge(rest, arrays))


def prepare_corpus(corpus: Corpus) -> PreparedCorpus:
    begin = time.monotonic()
    frames: list[PreparedFrame] = []
    for lines in corpus.lines_by_tu:
        if not lines:
            continue
        ordered = sorted(lines)
        baseline = serialize_front_split_sorted(ordered)
        candidate, stats = serialize_array_candidate(ordered)
        frames.append(
            PreparedFrame(
                ordered,
                baseline,
                candidate,
                stats["array_lines"],
                stats["array_text_bytes"],
                stats["array_values"],
                stats["styles"],
            )
        )
    return PreparedCorpus(corpus, frames, time.monotonic() - begin)


def compressed_part(compressor: zstd.ZstdCompressor, raw: bytes) -> bytes:
    return compressor.compress(raw)


def independent_best(prepared: PreparedCorpus, level: int) -> dict:
    compressor = zstd.ZstdCompressor(level=level)
    wire = baseline_wire = candidate_wire = 0
    baseline_wins = candidate_wins = 0
    component_wire = [0] * len(PART_NAMES)
    encode_seconds = decode_seconds = verification_seconds = 0.0

    for frame in prepared.frames:
        begin = time.monotonic()
        baseline_encoded = [compressed_part(compressor, raw) for raw in frame.baseline]
        candidate_encoded = [
            compressed_part(compressor, raw) if raw is not None else None
            for raw in frame.candidate
        ]
        encode_seconds += time.monotonic() - begin
        base_cost = sum(len(value) + 4 for value in baseline_encoded)
        candidate_cost = 1 + sum(
            len(value) + 4 for value in candidate_encoded if value is not None
        )
        baseline_wire += base_cost + 1
        candidate_wire += candidate_cost
        for index, value in enumerate(candidate_encoded):
            if value is not None:
                component_wire[index] += len(value) + 4

        # One byte selects baseline or generated-array form.  The baseline needs no other new field.
        use_candidate = candidate_cost < base_cost + 1
        decode_begin = time.monotonic()
        if use_candidate:
            candidate_raw = [
                zstd.ZstdDecompressor().decompress(value)
                if value is not None
                else None
                for value in candidate_encoded
            ]
            if decode_candidate(candidate_raw) != frame.expected:
                raise ValueError("selected generated-array Lines differ after decode")
        else:
            baseline_raw = [
                zstd.ZstdDecompressor().decompress(value)
                for value in baseline_encoded
            ]
            if decode_front_split(baseline_raw) != frame.expected:
                raise ValueError("selected baseline Lines differ after decode")
        decode_seconds += time.monotonic() - decode_begin

        # The losing representation is also independently decoded so a malformed candidate cannot
        # hide behind the actual-byte selector.  Keep this research validation out of product decode
        # timing.
        verification_begin = time.monotonic()
        if use_candidate:
            baseline_raw = [
                zstd.ZstdDecompressor().decompress(value)
                for value in baseline_encoded
            ]
            if decode_front_split(baseline_raw) != frame.expected:
                raise ValueError("losing baseline Lines differ after decode")
        else:
            candidate_raw = [
                zstd.ZstdDecompressor().decompress(value)
                if value is not None
                else None
                for value in candidate_encoded
            ]
            if decode_candidate(candidate_raw) != frame.expected:
                raise ValueError("losing generated-array Lines differ after decode")
        verification_seconds += time.monotonic() - verification_begin

        if use_candidate:
            wire += candidate_cost
            candidate_wins += 1
        else:
            wire += base_cost + 1
            baseline_wins += 1

    return {
        "wire_bytes": wire,
        "baseline_wire_bytes": baseline_wire,
        "candidate_wire_bytes": candidate_wire,
        "baseline_wins": baseline_wins,
        "candidate_wins": candidate_wins,
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "verification_seconds": verification_seconds,
        "component_wire": dict(zip(PART_NAMES, component_wire)),
        "exact": True,
    }


def persistent_stream(prepared: PreparedCorpus, level: int, generated: bool) -> dict:
    stream_count = len(PART_NAMES) if generated else 3
    compressors = [
        zstd.ZstdCompressor(level=level, write_content_size=False).compressobj()
        for _ in range(stream_count)
    ]
    encoded_frames: list[list[bytes | None]] = []
    last_active: list[int | None] = [None] * stream_count
    encode_begin = time.monotonic()
    for frame_index, frame in enumerate(prepared.frames):
        raw_parts: Sequence[bytes | None] = frame.candidate if generated else frame.baseline
        encoded: list[bytes | None] = []
        for index, (compressor, raw) in enumerate(zip(compressors, raw_parts)):
            if raw is None:
                encoded.append(None)
                continue
            encoded.append(
                compressor.compress(raw)
                + compressor.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
            )
            last_active[index] = frame_index
        encoded_frames.append(encoded)

    for index, compressor in enumerate(compressors):
        final_frame = last_active[index]
        if final_frame is not None:
            suffix = compressor.flush(zstd.COMPRESSOBJ_FLUSH_FINISH)
            current = encoded_frames[final_frame][index]
            if current is None:
                raise AssertionError("active stream lost its final frame")
            encoded_frames[final_frame][index] = current + suffix
    encode_seconds = time.monotonic() - encode_begin

    decompressors = [zstd.ZstdDecompressor().decompressobj() for _ in range(stream_count)]
    component_wire = [0] * stream_count
    decode_begin = time.monotonic()
    for frame, encoded in zip(prepared.frames, encoded_frames):
        recovered: list[bytes | None] = []
        for index, (decompressor, value) in enumerate(zip(decompressors, encoded)):
            if value is None:
                recovered.append(None)
                continue
            recovered.append(decompressor.decompress(value))
            component_wire[index] += len(value) + 4
        decoded = decode_candidate(recovered) if generated else decode_front_split(
            [part or b"" for part in recovered]
        )
        if decoded != frame.expected:
            raise ValueError("persistent Line stream differs after decode")
    decode_seconds = time.monotonic() - decode_begin

    for index, final_frame in enumerate(last_active):
        if final_frame is not None and not decompressors[index].eof:
            raise ValueError("persistent Line stream did not finish")
    selector_wire = len(prepared.frames) if generated else 0
    return {
        "wire_bytes": sum(component_wire) + selector_wire,
        "selector_wire_bytes": selector_wire,
        "encode_seconds": encode_seconds,
        "decode_seconds": decode_seconds,
        "component_wire": dict(zip(PART_NAMES[:stream_count], component_wire)),
        "exact": True,
    }


def summarize(prepared: PreparedCorpus, level: int) -> list[dict]:
    measurements = (
        ("independent-best", independent_best(prepared, level)),
        ("stream-split-front", persistent_stream(prepared, level, False)),
        ("stream-generated-array", persistent_stream(prepared, level, True)),
    )
    raw_bytes = sum(prepared.corpus.raw_by_tu)
    array_lines = sum(frame.array_lines for frame in prepared.frames)
    array_text_bytes = sum(frame.array_text_bytes for frame in prepared.frames)
    array_values = sum(frame.array_values for frame in prepared.frames)
    styles_max = max((frame.styles for frame in prepared.frames), default=0)
    rows = []
    for mode, measurement in measurements:
        wire = measurement["wire_bytes"]
        encode_seconds = measurement["encode_seconds"]
        decode_seconds = measurement["decode_seconds"]
        row = {
            "corpus": prepared.corpus.name,
            "level": level,
            "mode": mode,
            "tus": len(prepared.corpus.raw_by_tu),
            "frames": len(prepared.frames),
            "raw_bytes": raw_bytes,
            "line_bytes": prepared.corpus.line_bytes,
            "lines": sum(len(lines) for lines in prepared.corpus.lines_by_tu),
            "array_lines": array_lines,
            "array_text_bytes": array_text_bytes,
            "array_values": array_values,
            "maximum_styles_per_tu": styles_max,
            "wire_bytes": wire,
            "line_ratio": prepared.corpus.line_bytes / max(1, wire),
            "raw_ratio": raw_bytes / max(1, wire),
            "prepare_seconds": prepared.prepare_seconds,
            "encode_seconds": encode_seconds,
            "decode_seconds": decode_seconds,
            "effective_encode_GBps": raw_bytes
            / max(prepared.prepare_seconds + encode_seconds, 1e-12)
            / 1e9,
            "effective_decode_GBps": raw_bytes / max(decode_seconds, 1e-12) / 1e9,
            "prepare_line_GBps": prepared.corpus.line_bytes
            / max(prepared.prepare_seconds, 1e-12)
            / 1e9,
            "compression_encode_line_GBps": prepared.corpus.line_bytes
            / max(encode_seconds, 1e-12)
            / 1e9,
            "compression_encode_raw_GBps": raw_bytes
            / max(encode_seconds, 1e-12)
            / 1e9,
            "decode_line_GBps": prepared.corpus.line_bytes
            / max(decode_seconds, 1e-12)
            / 1e9,
            "exact": measurement["exact"],
            "component_wire": measurement.get("component_wire", {}),
        }
        for name in (
            "baseline_wire_bytes",
            "candidate_wire_bytes",
            "baseline_wins",
            "candidate_wins",
            "selector_wire_bytes",
            "verification_seconds",
        ):
            if name in measurement:
                row[name] = measurement[name]
        rows.append(row)
    return rows


def self_test() -> None:
    accepted = (
        b" 0, 1, 9, 10, 99, 100, 255,\n",
        b"\t1,2,3,4,\n",
        b"1, 2, 3, 4,\n",
    )
    rejected = (
        b"1, 2, 3,\n",
        b"01, 2, 3, 4,\n",
        b"1, 2, 256, 4,\n",
        b"1,  2, 3, 4,\n",
        b"1, 2, 3, 4\n",
        b"1, 2, 3, 4,\r\n",
        b"ordinary source line\n",
    )
    for line in accepted:
        parsed = parse_decimal_byte_line(line)
        if parsed is None or render_decimal_byte_line(parsed) != line:
            raise AssertionError(f"accepted decimal-byte self-test failed: {line!r}")
    for line in rejected:
        if parse_decimal_byte_line(line) is not None:
            raise AssertionError(f"rejected decimal-byte self-test failed: {line!r}")

    ordered = sorted((*accepted, b"alpha\n", b"omega\n"))
    parts, _ = serialize_array_candidate(ordered)
    if decode_candidate(parts) != ordered:
        raise AssertionError("generated-array candidate self-test differs")


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--trace", action="append", type=parse_trace, required=True)
    value.add_argument("--levels", nargs="+", type=int, default=(1, 3))
    value.add_argument("--max-tus", type=int, default=0)
    value.add_argument("--output", required=True)
    value.add_argument("--tsv")
    return value


def main() -> int:
    self_test()
    args = parser().parse_args()
    rows: list[dict] = []
    for name, path in args.trace:
        load_begin = time.monotonic()
        corpus = load_corpus(name, path, args.max_tus)
        load_seconds = time.monotonic() - load_begin
        prepared = prepare_corpus(corpus)
        print(
            f"LOAD {name} tus={len(corpus.raw_by_tu)} "
            f"lines={sum(map(len, corpus.lines_by_tu))} bytes={corpus.line_bytes} "
            f"load={load_seconds:.3f}s prepare={prepared.prepare_seconds:.3f}s",
            flush=True,
        )
        for level in args.levels:
            for row in summarize(prepared, level):
                row["load_seconds"] = load_seconds
                rows.append(row)
                print(
                    f"PASS {name} z{level} {row['mode']} wire={row['wire_bytes']} "
                    f"arrays={row['array_lines']}/{row['array_text_bytes']} "
                    f"values={row['array_values']} raw_ratio={row['raw_ratio']:.3f}",
                    flush=True,
                )

    report = {
        "experiment": "strict generated decimal-byte array Line program",
        "format": 1,
        "levels": args.levels,
        "max_tus": args.max_tus,
        "traces": [
            {"name": name, "path": path, "bytes": Path(path).stat().st_size}
            for name, path in args.trace
        ],
        "rows": rows,
    }
    Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.tsv and rows:
        scalar_names = sorted(
            {name for row in rows for name in row if name != "component_wire"}
        )
        with open(args.tsv, "w", newline="") as output:
            writer = csv.DictWriter(
                output, fieldnames=scalar_names, delimiter="\t", lineterminator="\n"
            )
            writer.writeheader()
            for row in rows:
                writer.writerow({name: row.get(name, "") for name in scalar_names})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
