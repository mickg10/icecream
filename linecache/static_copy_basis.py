#!/usr/bin/env python3
"""Train and test a portable exact byte-copy basis for PTGC raw definitions.

The training side reads only raw source roots.  The target side reads the raw-definition
channel emitted by ptgc_bench's SemanticFrameWriter.  Each target frame is represented as
ADD(exact bytes) and COPY_STATIC(offset, length) operations against one immutable zstd-trained
byte basis.  A separate decoder reconstructs every byte before any result is accepted.

This is deliberately a small capability probe.  A positive result justifies replacing frequently
used (offset, length) pairs with dense phrase IDs and adding the same operation to the causal online
dictionary.  A negative result rejects a static raw-source byte basis before building that layer.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import heapq
import json
import os
import struct
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterator, Sequence

import zstandard as zstd


FRAME_HEADER = struct.Struct("=8sII")
FRAME_RECORD = struct.Struct("=QI")
FRAME_MAGIC = b"ICFRM1\0\0"
SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp", ".tcc"
}
EXCLUDED_DIRS = {
    ".git", ".hg", ".svn", "build", "build32", "build64", "cmake-build-debug",
    "cmake-build-release", "node_modules", "third_party_build", "_build",
}


def put_varint(out: bytearray, value: int) -> None:
    if value < 0:
        raise ValueError("negative varint")
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)


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
    raise ValueError("truncated or overlong varint")


def varint_size(value: int) -> int:
    size = 1
    while value >= 0x80:
        size += 1
        value >>= 7
    return size


@dataclasses.dataclass(frozen=True)
class TargetFrame:
    raw_input_bytes: int
    payload: bytes


def read_frames(path: Path, maximum: int) -> list[TargetFrame]:
    frames: list[TargetFrame] = []
    with path.open("rb") as source:
        header = source.read(FRAME_HEADER.size)
        if len(header) != FRAME_HEADER.size:
            raise ValueError(f"{path}: short frame header")
        magic, version, count = FRAME_HEADER.unpack(header)
        if magic != FRAME_MAGIC or version != 1:
            raise ValueError(f"{path}: unknown frame format")
        for _ in range(min(count, maximum)):
            record = source.read(FRAME_RECORD.size)
            if len(record) != FRAME_RECORD.size:
                raise ValueError(f"{path}: short frame record")
            raw_bytes, size = FRAME_RECORD.unpack(record)
            payload = source.read(size)
            if len(payload) != size:
                raise ValueError(f"{path}: short frame payload")
            frames.append(TargetFrame(raw_bytes, payload))
    return frames


def source_files(root: Path) -> Iterator[Path]:
    for current, directories, files in os.walk(root):
        directories[:] = sorted(name for name in directories if name not in EXCLUDED_DIRS)
        for name in sorted(files):
            path = Path(current, name)
            if path.suffix.lower() in SOURCE_SUFFIXES:
                yield path


def line_samples(root: Path, cap: int, divisor: int) -> tuple[list[bytes], dict[str, int]]:
    # Keep the lowest deterministic hashes.  The preliminary divisor avoids millions of heap
    # operations while leaving enough material to fill the per-project cap on the measured roots.
    retained: list[tuple[int, int, bytes]] = []
    retained_bytes = 0
    files = lines = eligible = 0
    sequence = 0
    for path in source_files(root):
        files += 1
        try:
            data = path.read_bytes()
        except OSError:
            continue
        for line in data.splitlines(keepends=True):
            lines += 1
            if len(line) < 8 or len(line) > 16_384:
                continue
            digest = hashlib.blake2b(line, digest_size=8, person=b"ice-phr1").digest()
            key = int.from_bytes(digest, "little")
            if key % divisor:
                continue
            eligible += 1
            item = (-key, -sequence, line)
            sequence += 1
            heapq.heappush(retained, item)
            retained_bytes += len(line)
            while retained_bytes > cap and retained:
                _, _, removed = heapq.heappop(retained)
                retained_bytes -= len(removed)
    ordered = sorted(((-key, -seq, value) for key, seq, value in retained))
    samples = [value for _, _, value in ordered]
    return samples, {
        "files": files,
        "lines": lines,
        "eligible": eligible,
        "samples": len(samples),
        "bytes": sum(map(len, samples)),
    }


def collect_samples(roots: Sequence[Path], cap: int, divisor: int) -> tuple[list[bytes], list[dict]]:
    all_samples: list[bytes] = []
    reports: list[dict] = []
    for root in roots:
        samples, report = line_samples(root, cap, divisor)
        report["root"] = str(root)
        reports.append(report)
        all_samples.extend(samples)
        print(
            f"sample {root}: files={report['files']} lines={report['lines']} "
            f"kept={report['samples']} bytes={report['bytes']}",
            file=sys.stderr,
        )
    return all_samples, reports


class StaticMatcher:
    def __init__(self, basis: bytes, seed_bytes: int, maximum_candidates: int):
        if seed_bytes != 4:
            raise ValueError("the current probe uses four-byte seeds")
        self.basis = basis
        self.seed_bytes = seed_bytes
        positions: dict[int, list[int]] = defaultdict(list)
        for offset in range(max(0, len(basis) - seed_bytes + 1)):
            positions[int.from_bytes(basis[offset:offset + seed_bytes], "little")].append(offset)
        self.positions: dict[int, tuple[int, ...]] = {}
        for key, values in positions.items():
            if len(values) <= maximum_candidates:
                selected = values
            else:
                step = (len(values) + maximum_candidates - 1) // maximum_candidates
                selected = values[::step][:maximum_candidates - 1] + [values[-1]]
            self.positions[key] = tuple(dict.fromkeys(selected))

    def longest(self, target: bytes, position: int) -> tuple[int, int]:
        if position + self.seed_bytes > len(target):
            return 0, 0
        key = int.from_bytes(target[position:position + self.seed_bytes], "little")
        best_offset = best_length = 0
        for basis_offset in self.positions.get(key, ()):
            length = self.seed_bytes
            maximum = min(len(target) - position, len(self.basis) - basis_offset)
            while length + 8 <= maximum and (
                target[position + length:position + length + 8]
                == self.basis[basis_offset + length:basis_offset + length + 8]
            ):
                length += 8
            while length < maximum and target[position + length] == self.basis[basis_offset + length]:
                length += 1
            if length > best_length:
                best_offset, best_length = basis_offset, length
        return best_offset, best_length


@dataclasses.dataclass(frozen=True)
class Operation:
    copy: bool
    offset: int
    length: int


@dataclasses.dataclass
class Program:
    control: bytes
    residual: bytes
    operations: list[Operation]
    covered: int


def encode_program(target: bytes, matcher: StaticMatcher, minimum_match: int) -> Program:
    operations: list[Operation] = []
    residual = bytearray()
    position = literal_begin = 0
    covered = 0
    while position < len(target):
        basis_offset, length = matcher.longest(target, position)
        copy_cost = varint_size((length << 1) | 1) + varint_size(basis_offset)
        if length >= minimum_match and copy_cost < length:
            if literal_begin < position:
                chunk = target[literal_begin:position]
                operations.append(Operation(False, len(residual), len(chunk)))
                residual.extend(chunk)
            operations.append(Operation(True, basis_offset, length))
            covered += length
            position += length
            literal_begin = position
        else:
            position += 1
    if literal_begin < len(target):
        chunk = target[literal_begin:]
        operations.append(Operation(False, len(residual), len(chunk)))
        residual.extend(chunk)

    control = bytearray()
    put_varint(control, len(target))
    put_varint(control, len(operations))
    for operation in operations:
        put_varint(control, (operation.length << 1) | int(operation.copy))
        if operation.copy:
            put_varint(control, operation.offset)
    return Program(bytes(control), bytes(residual), operations, covered)


def decode_program(program: Program, basis: bytes) -> bytes:
    output_length, position = get_varint(program.control, 0)
    operation_count, position = get_varint(program.control, position)
    output = bytearray()
    residual_position = 0
    for _ in range(operation_count):
        code, position = get_varint(program.control, position)
        length = code >> 1
        if not length:
            raise ValueError("zero-length operation")
        if code & 1:
            offset, position = get_varint(program.control, position)
            if offset > len(basis) or length > len(basis) - offset:
                raise ValueError("static copy exceeds basis")
            output.extend(basis[offset:offset + length])
        else:
            if length > len(program.residual) - residual_position:
                raise ValueError("add exceeds residual")
            output.extend(program.residual[residual_position:residual_position + length])
            residual_position += length
    if position != len(program.control) or residual_position != len(program.residual):
        raise ValueError("trailing program bytes")
    if len(output) != output_length:
        raise ValueError("program output length mismatch")
    return bytes(output)


@dataclasses.dataclass(frozen=True)
class WireCandidate:
    name: str
    wire: int
    compressed: tuple[bytes, ...]


def compressed_candidates(
    program: Program, level: int, model: bytes, *, raw_dictionary: bool = False
) -> list[WireCandidate]:
    plain = zstd.ZstdCompressor(level=level)
    dictionary = zstd.ZstdCompressionDict(
        model, dict_type=zstd.DICT_TYPE_RAWCONTENT if raw_dictionary else zstd.DICT_TYPE_AUTO
    )
    with_dictionary = zstd.ZstdCompressor(level=level, dict_data=dictionary)
    combined = bytearray()
    put_varint(combined, len(program.control))
    combined.extend(program.control)
    combined.extend(program.residual)
    candidates: list[WireCandidate] = []
    for name, compressor in (("plain", plain), ("cdict", with_dictionary)):
        one = compressor.compress(bytes(combined))
        candidates.append(WireCandidate(f"{name}-combined", len(one) + 4, (one,)))
        control = compressor.compress(program.control)
        residual = compressor.compress(program.residual) if program.residual else b""
        wire = len(control) + 4 + (len(residual) + 4 if residual else 0)
        candidates.append(WireCandidate(f"{name}-split", wire, (control, residual)))
    return candidates


def verify_candidate(
    candidate: WireCandidate, program: Program, level: int, model: bytes, *,
    raw_dictionary: bool = False,
) -> None:
    del level
    dictionary = zstd.ZstdCompressionDict(
        model, dict_type=zstd.DICT_TYPE_RAWCONTENT if raw_dictionary else zstd.DICT_TYPE_AUTO
    )
    decompressor = (
        zstd.ZstdDecompressor(dict_data=dictionary)
        if candidate.name.startswith("cdict") else zstd.ZstdDecompressor()
    )
    if candidate.name.endswith("combined"):
        combined = decompressor.decompress(candidate.compressed[0])
        control_size, position = get_varint(combined, 0)
        if control_size > len(combined) - position:
            raise ValueError("combined control exceeds frame")
        control = combined[position:position + control_size]
        residual = combined[position + control_size:]
    else:
        control = decompressor.decompress(candidate.compressed[0])
        residual = (
            decompressor.decompress(candidate.compressed[1]) if candidate.compressed[1] else b""
        )
    if control != program.control or residual != program.residual:
        raise ValueError("compressed program round trip mismatch")


@dataclasses.dataclass
class Score:
    minimum_match: int
    literal_wire: int = 0
    candidate_wire: int = 0
    selected_wire: int = 0
    wins: int = 0
    covered: int = 0
    target_bytes: int = 0
    copies: int = 0
    adds: int = 0
    layout_wins: dict[str, int] = dataclasses.field(default_factory=dict)


def score_target(
    frames: Sequence[TargetFrame], model: bytes, level: int, minimum_match: int,
    maximum_candidates: int,
) -> Score:
    matcher = StaticMatcher(model, 4, maximum_candidates)
    compressor = zstd.ZstdCompressor(level=level)
    score = Score(minimum_match)
    for frame in frames:
        literal_wire = len(compressor.compress(frame.payload)) + 4
        program = encode_program(frame.payload, matcher, minimum_match)
        if decode_program(program, model) != frame.payload:
            raise ValueError("independent static-copy decode mismatch")
        candidates = compressed_candidates(program, level, model)
        winner = min(candidates, key=lambda candidate: (candidate.wire, candidate.name))
        verify_candidate(winner, program, level, model)
        use_program = winner.wire < literal_wire
        score.literal_wire += literal_wire
        score.candidate_wire += winner.wire
        score.selected_wire += min(literal_wire, winner.wire) + 1
        score.wins += int(use_program)
        score.covered += program.covered
        score.target_bytes += len(frame.payload)
        score.copies += sum(operation.copy for operation in program.operations)
        score.adds += sum(not operation.copy for operation in program.operations)
        score.layout_wins[winner.name] = score.layout_wins.get(winner.name, 0) + 1
    return score


def train_model(samples: Sequence[bytes], kib: int) -> bytes:
    if sum(map(len, samples)) < kib * 1024 * 8:
        raise ValueError("not enough training bytes for requested dictionary")
    return zstd.train_dictionary(kib * 1024, samples).as_bytes()


def byte_decoder() -> dict[str, int]:
    # GPT-2/ByteLevel's reversible byte alphabet.  The tokenizers package exposes the alphabet but
    # not the byte mapping, so reconstruct the documented mapping deterministically.
    byte_values = list(range(ord("!"), ord("~") + 1))
    byte_values += list(range(0xA1, 0xAC + 1))
    byte_values += list(range(0xAE, 0xFF + 1))
    characters = list(byte_values)
    missing = 0
    for value in range(256):
        if value not in byte_values:
            byte_values.append(value)
            characters.append(256 + missing)
            missing += 1
    return {chr(character): value for value, character in zip(byte_values, characters)}


@dataclasses.dataclass(frozen=True)
class PhraseCandidate:
    value: bytes
    count: int
    score: int


def train_bpe_candidates(
    samples: Sequence[bytes], vocabulary_size: int, minimum_frequency: int,
) -> tuple[list[PhraseCandidate], dict]:
    from tokenizers import Tokenizer, models, pre_tokenizers, trainers

    ascii_samples = [sample.decode("ascii") for sample in samples if sample.isascii()]
    tokenizer = Tokenizer(models.BPE())
    tokenizer.pre_tokenizer = pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=False)
    trainer = trainers.BpeTrainer(
        vocab_size=vocabulary_size,
        min_frequency=minimum_frequency,
        show_progress=False,
        initial_alphabet=pre_tokenizers.ByteLevel.alphabet(),
    )
    tokenizer.train_from_iterator(ascii_samples, trainer=trainer, length=len(ascii_samples))
    counts: dict[int, int] = defaultdict(int)
    for begin in range(0, len(ascii_samples), 4096):
        for encoding in tokenizer.encode_batch(ascii_samples[begin:begin + 4096]):
            for token_id in encoding.ids:
                counts[token_id] += 1
    inverse = byte_decoder()
    candidates: list[PhraseCandidate] = []
    for token, token_id in tokenizer.get_vocab().items():
        try:
            value = bytes(inverse[character] for character in token)
        except KeyError:
            continue
        count = counts.get(token_id, 0)
        if len(value) < 2 or count < minimum_frequency:
            continue
        package_cost = varint_size(len(value)) + len(value)
        score = count * (len(value) - 1) - package_cost
        if score > 0:
            candidates.append(PhraseCandidate(value, count, score))
    candidates.sort(key=lambda candidate: (-candidate.score, -candidate.count, -len(candidate.value), candidate.value))
    return candidates, {
        "ascii_samples": len(ascii_samples),
        "ascii_bytes": sum(len(sample) for sample in ascii_samples),
        "trained_vocabulary": tokenizer.get_vocab_size(),
        "retained_candidates": len(candidates),
    }


@dataclasses.dataclass(frozen=True)
class PhrasePackage:
    phrases: tuple[bytes, ...]
    raw: bytes


def make_phrase_package(candidates: Sequence[PhraseCandidate], budget: int) -> PhrasePackage:
    selected: list[bytes] = []
    used = 16
    for candidate in candidates:
        cost = varint_size(len(candidate.value)) + len(candidate.value)
        if used + cost <= budget:
            selected.append(candidate.value)
            used += cost
    raw = bytearray()
    put_varint(raw, 1)
    put_varint(raw, len(selected))
    for phrase in selected:
        put_varint(raw, len(phrase))
        raw.extend(phrase)
    if len(raw) > budget:
        raise ValueError("phrase package exceeded budget")
    return PhrasePackage(tuple(selected), bytes(raw))


def decode_phrase_package(raw: bytes) -> tuple[bytes, ...]:
    version, position = get_varint(raw, 0)
    if version != 1:
        raise ValueError("unknown phrase package version")
    count, position = get_varint(raw, position)
    phrases = []
    for _ in range(count):
        length, position = get_varint(raw, position)
        if length > len(raw) - position:
            raise ValueError("phrase exceeds package")
        phrases.append(raw[position:position + length])
        position += length
    if position != len(raw):
        raise ValueError("trailing phrase package bytes")
    return tuple(phrases)


class PhraseMatcher:
    def __init__(self, phrases: Sequence[bytes]):
        self.phrases = phrases
        self.children: list[dict[int, int]] = [{}]
        self.terminals: list[list[int]] = [[]]
        for phrase_id, phrase in enumerate(phrases):
            node = 0
            for byte in phrase:
                child = self.children[node].get(byte)
                if child is None:
                    child = len(self.children)
                    self.children[node][byte] = child
                    self.children.append({})
                    self.terminals.append([])
                node = child
            self.terminals[node].append(phrase_id)

    def matches(self, target: bytes, position: int) -> Iterator[tuple[int, int]]:
        node = 0
        for end in range(position, len(target)):
            child = self.children[node].get(target[end])
            if child is None:
                return
            node = child
            for phrase_id in self.terminals[node]:
                yield phrase_id, end + 1 - position


@dataclasses.dataclass
class PhraseProgram:
    control: bytes
    residual: bytes
    phrase_ops: int
    add_ops: int
    covered: int
    dense: bool = False


def encode_phrase_program(
    target: bytes, matcher: PhraseMatcher, planner: str = "byte-cost",
) -> PhraseProgram:
    # Literal steps cost one byte plus a one-byte opening approximation.  The emitted program below
    # merges adjacent literal steps and records the exact varint length; actual compressed buffers,
    # not this planning score, decide whether the candidate is selected.
    infinity = 1 << 60
    cost = [infinity] * (len(target) + 1)
    previous = [-1] * (len(target) + 1)
    phrase_at = [-1] * (len(target) + 1)
    literal_open = [False] * (len(target) + 1)
    cost[0] = 0
    for position in range(len(target)):
        literal_cost = cost[position] + 1 + (0 if literal_open[position] else 1)
        if literal_cost < cost[position + 1]:
            cost[position + 1] = literal_cost
            previous[position + 1] = position
            phrase_at[position + 1] = -1
            literal_open[position + 1] = True
        for phrase_id, length in matcher.matches(target, position):
            next_position = position + length
            encoded_id_cost = varint_size((phrase_id << 1) | 1)
            phrase_cost = cost[position] + (encoded_id_cost if planner == "byte-cost" else 1)
            if phrase_cost < cost[next_position]:
                cost[next_position] = phrase_cost
                previous[next_position] = position
                phrase_at[next_position] = phrase_id
                literal_open[next_position] = False

    steps: list[tuple[int, int, int]] = []
    position = len(target)
    while position:
        begin = previous[position]
        if begin < 0:
            raise ValueError("phrase planner did not reach output")
        steps.append((begin, position, phrase_at[position]))
        position = begin
    steps.reverse()
    merged: list[tuple[int, int, int]] = []
    for begin, end, phrase_id in steps:
        if phrase_id < 0 and merged and merged[-1][2] < 0 and merged[-1][1] == begin:
            prior = merged[-1]
            merged[-1] = (prior[0], end, -1)
        else:
            merged.append((begin, end, phrase_id))

    control = bytearray()
    residual = bytearray()
    put_varint(control, len(target))
    put_varint(control, len(merged))
    phrase_ops = add_ops = covered = 0
    for begin, end, phrase_id in merged:
        if phrase_id < 0:
            put_varint(control, (end - begin) << 1)
            residual.extend(target[begin:end])
            add_ops += 1
        else:
            put_varint(control, (phrase_id << 1) | 1)
            phrase_ops += 1
            covered += end - begin
    return PhraseProgram(bytes(control), bytes(residual), phrase_ops, add_ops, covered)


def decode_phrase_program(program: PhraseProgram, phrases: Sequence[bytes]) -> bytes:
    output_length, position = get_varint(program.control, 0)
    palette: list[int] = []
    if program.dense:
        palette_count, position = get_varint(program.control, position)
        for _ in range(palette_count):
            phrase_id, position = get_varint(program.control, position)
            if phrase_id >= len(phrases):
                raise ValueError("dense palette contains unknown phrase")
            palette.append(phrase_id)
    operation_count, position = get_varint(program.control, position)
    output = bytearray()
    residual_position = 0
    for _ in range(operation_count):
        code, position = get_varint(program.control, position)
        if code & 1:
            phrase_id = code >> 1
            if program.dense:
                if phrase_id >= len(palette):
                    raise ValueError("unknown dense phrase id")
                phrase_id = palette[phrase_id]
            if phrase_id >= len(phrases):
                raise ValueError("unknown phrase id")
            output.extend(phrases[phrase_id])
        else:
            length = code >> 1
            if not length or length > len(program.residual) - residual_position:
                raise ValueError("phrase ADD exceeds residual")
            output.extend(program.residual[residual_position:residual_position + length])
            residual_position += length
    if position != len(program.control) or residual_position != len(program.residual):
        raise ValueError("trailing phrase program bytes")
    if len(output) != output_length:
        raise ValueError("phrase output length mismatch")
    return bytes(output)


def dense_phrase_program(program: PhraseProgram) -> PhraseProgram:
    if program.dense:
        return program
    output_length, position = get_varint(program.control, 0)
    operation_count, position = get_varint(program.control, position)
    operations = []
    frequency: Counter[int] = Counter()
    for _ in range(operation_count):
        code, position = get_varint(program.control, position)
        operations.append(code)
        if code & 1:
            frequency[code >> 1] += 1
    if position != len(program.control):
        raise ValueError("trailing global phrase control")
    palette = sorted(frequency, key=lambda phrase_id: (-frequency[phrase_id], phrase_id))
    local = {phrase_id: local_id for local_id, phrase_id in enumerate(palette)}
    control = bytearray()
    put_varint(control, output_length)
    put_varint(control, len(palette))
    for phrase_id in palette:
        put_varint(control, phrase_id)
    put_varint(control, operation_count)
    for code in operations:
        put_varint(control, (local[code >> 1] << 1) | 1 if code & 1 else code)
    return PhraseProgram(
        bytes(control), program.residual, program.phrase_ops, program.add_ops, program.covered, True
    )


def score_phrase_target(
    frames: Sequence[TargetFrame], package: PhrasePackage, level: int,
) -> dict:
    receiver_phrases = decode_phrase_package(package.raw)
    if receiver_phrases != package.phrases:
        raise ValueError("phrase package reconstruction mismatch")
    matcher = PhraseMatcher(package.phrases)
    compressor = zstd.ZstdCompressor(level=level)
    totals = {
        "literal_wire": 0,
        "candidate_wire": 0,
        "selected_wire": 0,
        "wins": 0,
        "covered": 0,
        "target_bytes": 0,
        "phrase_ops": 0,
        "add_ops": 0,
        "layout_wins": {},
    }
    for frame in frames:
        literal_wire = len(compressor.compress(frame.payload)) + 4
        alternatives = []
        for planner in ("byte-cost", "fewest-ops"):
            global_program = encode_phrase_program(frame.payload, matcher, planner)
            for mapping, phrase_program in (
                ("global", global_program), ("dense", dense_phrase_program(global_program))
            ):
                if decode_phrase_program(phrase_program, receiver_phrases) != frame.payload:
                    raise ValueError("independent dense-phrase decode mismatch")
                generic_program = Program(
                    phrase_program.control, phrase_program.residual, [], phrase_program.covered
                )
                for candidate in compressed_candidates(
                    generic_program, level, package.raw, raw_dictionary=True
                ):
                    alternatives.append((candidate, phrase_program, generic_program, planner, mapping))
        winner, phrase_program, generic_program, planner, mapping = min(
            alternatives,
            key=lambda alternative: (
                alternative[0].wire, alternative[3], alternative[4], alternative[0].name
            ),
        )
        verify_candidate(
            winner, generic_program, level, package.raw, raw_dictionary=True
        )
        totals["literal_wire"] += literal_wire
        totals["candidate_wire"] += winner.wire
        totals["selected_wire"] += min(literal_wire, winner.wire) + 1
        totals["wins"] += int(winner.wire < literal_wire)
        totals["covered"] += phrase_program.covered
        totals["target_bytes"] += len(frame.payload)
        totals["phrase_ops"] += phrase_program.phrase_ops
        totals["add_ops"] += phrase_program.add_ops
        layouts = totals["layout_wins"]
        layout = f"{planner}/{mapping}/{winner.name}"
        layouts[layout] = layouts.get(layout, 0) + 1
    return totals


def plain_compressed_candidates(program: Program, level: int) -> list[WireCandidate]:
    compressor = zstd.ZstdCompressor(level=level)
    combined = bytearray()
    put_varint(combined, len(program.control))
    combined.extend(program.control)
    combined.extend(program.residual)
    one = compressor.compress(bytes(combined))
    control = compressor.compress(program.control)
    residual = compressor.compress(program.residual) if program.residual else b""
    split_wire = len(control) + 4 + (len(residual) + 4 if residual else 0)
    return [
        WireCandidate("plain-combined", len(one) + 4, (one,)),
        WireCandidate("plain-split", split_wire, (control, residual)),
    ]


class OnlinePhraseLearner:
    def __init__(
        self, budget: int, promotions_per_round: int, minimum_count: int,
        maximum_length: int, learning_rounds: int, history_bytes: int,
    ):
        self.budget = budget
        self.promotions_per_round = promotions_per_round
        self.minimum_count = minimum_count
        self.maximum_length = maximum_length
        self.learning_rounds = learning_rounds
        self.history_bytes = history_bytes
        self.phrases: list[bytes] = []
        self.by_value: dict[bytes, int] = {}
        self.promoted_pairs: set[tuple[int, int]] = set()
        self.phrase_bytes = 0
        self.matcher = PhraseMatcher(())
        self.history: list[bytes] = []
        self.history_size = 0

    @staticmethod
    def byte_token(value: int) -> int:
        return -value - 1

    def expansion(self, token: int) -> bytes:
        return bytes((-token - 1,)) if token < 0 else self.phrases[token]

    def tokens(self, payload: bytes) -> list[int]:
        tokens = []
        position = 0
        while position < len(payload):
            best_id = -1
            best_length = 0
            for phrase_id, length in self.matcher.matches(payload, position):
                if length > best_length or (length == best_length and phrase_id < best_id):
                    best_id, best_length = phrase_id, length
            if best_id >= 0:
                tokens.append(best_id)
                position += best_length
            else:
                tokens.append(self.byte_token(payload[position]))
                position += 1
        return tokens

    def observe(self, payload: bytes) -> int:
        self.history.append(payload)
        self.history_size += len(payload)
        while len(self.history) > 1 and self.history_size > self.history_bytes:
            self.history_size -= len(self.history.pop(0))
        total_promoted = 0
        for _ in range(self.learning_rounds):
            pair_counts: Counter[tuple[int, int]] = Counter()
            for prior in self.history:
                tokens = self.tokens(prior)
                pair_counts.update(zip(tokens, tokens[1:]))
            candidates = sorted(
                (
                    (count, pair, self.expansion(pair[0]) + self.expansion(pair[1]))
                    for pair, count in pair_counts.items()
                    if count >= self.minimum_count and pair not in self.promoted_pairs
                ),
                key=lambda item: (-item[0], -len(item[2]), item[2], item[1]),
            )
            round_promoted = 0
            for _, pair, value in candidates:
                self.promoted_pairs.add(pair)
                if len(value) > self.maximum_length or value in self.by_value:
                    continue
                if self.phrase_bytes + len(value) > self.budget:
                    continue
                self.by_value[value] = len(self.phrases)
                self.phrases.append(value)
                self.phrase_bytes += len(value)
                round_promoted += 1
                total_promoted += 1
                if round_promoted >= self.promotions_per_round:
                    break
            if not round_promoted:
                break
            self.matcher = PhraseMatcher(self.phrases)
        return total_promoted

    def identity(self) -> str:
        digest = hashlib.sha256()
        for phrase in self.phrases:
            digest.update(struct.pack("=I", len(phrase)))
            digest.update(phrase)
        return digest.hexdigest()


def encode_online_frame(payload: bytes, learner: OnlinePhraseLearner, level: int) -> tuple[
    PhraseProgram, WireCandidate, str
]:
    alternatives = []
    for planner in ("byte-cost", "fewest-ops"):
        global_program = encode_phrase_program(payload, learner.matcher, planner)
        for mapping, phrase_program in (
            ("global", global_program), ("dense", dense_phrase_program(global_program))
        ):
            generic_program = Program(
                phrase_program.control, phrase_program.residual, [], phrase_program.covered
            )
            for candidate in plain_compressed_candidates(generic_program, level):
                alternatives.append((candidate, phrase_program, planner, mapping))
    candidate, program, planner, mapping = min(
        alternatives,
        key=lambda alternative: (
            alternative[0].wire, alternative[2], alternative[3], alternative[0].name
        ),
    )
    return program, candidate, f"{planner}/{mapping}"


def decode_plain_candidate(candidate: WireCandidate) -> Program:
    decompressor = zstd.ZstdDecompressor()
    if candidate.name.endswith("combined"):
        combined = decompressor.decompress(candidate.compressed[0])
        control_size, position = get_varint(combined, 0)
        if control_size > len(combined) - position:
            raise ValueError("online combined control exceeds frame")
        control = combined[position:position + control_size]
        residual = combined[position + control_size:]
    else:
        control = decompressor.decompress(candidate.compressed[0])
        residual = (
            decompressor.decompress(candidate.compressed[1]) if candidate.compressed[1] else b""
        )
    return Program(control, residual, [], 0)


def score_online_target(
    frames: Sequence[TargetFrame], level: int, budget: int, promotions_per_round: int,
    minimum_count: int, maximum_length: int, learning_rounds: int, history_bytes: int,
) -> dict:
    encoder = OnlinePhraseLearner(
        budget, promotions_per_round, minimum_count, maximum_length, learning_rounds, history_bytes
    )
    decoder = OnlinePhraseLearner(
        budget, promotions_per_round, minimum_count, maximum_length, learning_rounds, history_bytes
    )
    compressor = zstd.ZstdCompressor(level=level)
    result = {
        "literal_wire": 0,
        "candidate_wire": 0,
        "selected_wire": 0,
        "wins": 0,
        "covered": 0,
        "target_bytes": 0,
        "phrase_ops": 0,
        "add_ops": 0,
        "promoted": 0,
        "layout_wins": {},
        "curve": [],
    }
    cumulative_raw = 0
    for ordinal, frame in enumerate(frames, 1):
        if encoder.phrases != decoder.phrases:
            raise ValueError("online phrase states diverged before frame")
        literal_wire = len(compressor.compress(frame.payload)) + 4
        phrase_program, candidate, planner = encode_online_frame(frame.payload, encoder, level)
        transported = decode_plain_candidate(candidate)
        receiver_program = PhraseProgram(
            transported.control, transported.residual, 0, 0, 0, phrase_program.dense
        )
        reconstructed = decode_phrase_program(receiver_program, decoder.phrases)
        if reconstructed != frame.payload:
            raise ValueError("independent online phrase decode mismatch")
        use_program = candidate.wire < literal_wire
        result["literal_wire"] += literal_wire
        result["candidate_wire"] += candidate.wire
        result["selected_wire"] += min(literal_wire, candidate.wire) + 1
        result["wins"] += int(use_program)
        result["covered"] += phrase_program.covered
        result["target_bytes"] += len(frame.payload)
        result["phrase_ops"] += phrase_program.phrase_ops
        result["add_ops"] += phrase_program.add_ops
        layout = f"{planner}/{candidate.name}"
        layouts = result["layout_wins"]
        layouts[layout] = layouts.get(layout, 0) + 1
        promoted_encoder = encoder.observe(frame.payload)
        promoted_decoder = decoder.observe(reconstructed)
        if promoted_encoder != promoted_decoder or encoder.identity() != decoder.identity():
            raise ValueError("online phrase learners diverged after frame")
        result["promoted"] += promoted_encoder
        cumulative_raw += frame.raw_input_bytes
        result["curve"].append({
            "tu": ordinal,
            "raw_input_bytes": cumulative_raw,
            "literal_wire": result["literal_wire"],
            "selected_wire": result["selected_wire"],
            "ratio_literal": cumulative_raw / max(1, result["literal_wire"]),
            "ratio_online": cumulative_raw / max(1, result["selected_wire"]),
            "phrases": len(encoder.phrases),
            "phrase_bytes": encoder.phrase_bytes,
            "model_id": encoder.identity()[:16],
        })
    result["phrases"] = len(encoder.phrases)
    result["phrase_bytes"] = encoder.phrase_bytes
    result["model_sha256"] = encoder.identity()
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", action="append", type=Path, default=[])
    parser.add_argument("--target", action="append", nargs=2, metavar=("NAME", "RAW_FRAMES"), required=True)
    parser.add_argument("--dict-kib", nargs="+", type=int, default=[16, 64, 256])
    parser.add_argument("--skip-copy-basis", action="store_true")
    parser.add_argument("--phrase-kib", nargs="+", type=int, default=[])
    parser.add_argument("--bpe-vocabulary", type=int, default=8192)
    parser.add_argument("--bpe-minimum-frequency", type=int, default=3)
    parser.add_argument("--online-phrase-kib", nargs="+", type=int, default=[])
    parser.add_argument("--online-promotions", nargs="+", type=int, default=[256])
    parser.add_argument("--online-minimum-count", nargs="+", type=int, default=[4])
    parser.add_argument("--online-maximum-length", type=int, default=256)
    parser.add_argument("--online-rounds", nargs="+", type=int, default=[4])
    parser.add_argument("--online-history-mib", type=int, default=64)
    parser.add_argument("--sample-mib-per-root", type=int, default=4)
    parser.add_argument("--sample-divisor", type=int, default=8)
    parser.add_argument("--max-tus", type=int, default=20)
    parser.add_argument("--minimum-match", nargs="+", type=int, default=[4, 6, 8, 12])
    parser.add_argument("--maximum-candidates", type=int, default=32)
    parser.add_argument("--z", type=int, default=3)
    parser.add_argument("--output-prefix", type=Path, default=Path("/tmp/ptgc-static-copy"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if args.sample_divisor <= 0 or args.max_tus <= 0:
        parser.error("sampling divisor and max TUs must be positive")
    if (not args.skip_copy_basis or args.phrase_kib) and not args.source_root:
        parser.error("static copy/BPE training requires at least one --source-root")

    started = time.perf_counter()
    if not args.skip_copy_basis or args.phrase_kib:
        samples, sample_reports = collect_samples(
            args.source_root, args.sample_mib_per_root * 1024 * 1024, args.sample_divisor
        )
    else:
        samples, sample_reports = [], []
    targets = {
        name: read_frames(Path(path), args.max_tus)
        for name, path in args.target
    }
    report: dict = {
        "format": 1,
        "source_roots": [str(root) for root in args.source_root],
        "samples": sample_reports,
        "sample_count": len(samples),
        "sample_bytes": sum(map(len, samples)),
        "zstd_level": args.z,
        "max_tus": args.max_tus,
        "models": [],
        "phrase_models": [],
        "online_models": [],
    }
    for kib in (() if args.skip_copy_basis else args.dict_kib):
        model_started = time.perf_counter()
        model = train_model(samples, kib)
        model_path = Path(f"{args.output_prefix}-{kib}k.dict")
        model_path.write_bytes(model)
        model_wire = len(zstd.ZstdCompressor(level=args.z).compress(model)) + 4
        model_report = {
            "requested_kib": kib,
            "model_bytes": len(model),
            "model_wire": model_wire,
            "sha256": hashlib.sha256(model).hexdigest(),
            "targets": {},
        }
        for name, frames in targets.items():
            rows = []
            for minimum_match in args.minimum_match:
                score = score_target(
                    frames, model, args.z, minimum_match, args.maximum_candidates
                )
                rows.append(dataclasses.asdict(score))
                print(
                    f"{kib:4d} KiB {name:12s} min={minimum_match:2d} "
                    f"literal/candidate/selected={score.literal_wire}/{score.candidate_wire}/"
                    f"{score.selected_wire} covered={score.covered}/{score.target_bytes} "
                    f"ops copy/add={score.copies}/{score.adds} wins={score.wins}/{len(frames)} "
                    f"layouts={dict(score.layout_wins)}",
                    file=sys.stderr,
                )
            model_report["targets"][name] = rows
        model_report["seconds"] = time.perf_counter() - model_started
        report["models"].append(model_report)
    if args.phrase_kib:
        phrase_started = time.perf_counter()
        phrase_candidates, bpe_report = train_bpe_candidates(
            samples, args.bpe_vocabulary, args.bpe_minimum_frequency
        )
        report["bpe_training"] = bpe_report
        for kib in args.phrase_kib:
            package = make_phrase_package(phrase_candidates, kib * 1024)
            package_path = Path(f"{args.output_prefix}-phrases-{kib}k.package")
            package_path.write_bytes(package.raw)
            package_wire = len(zstd.ZstdCompressor(level=args.z).compress(package.raw)) + 4
            package_report = {
                "requested_kib": kib,
                "phrases": len(package.phrases),
                "package_bytes": len(package.raw),
                "package_wire": package_wire,
                "sha256": hashlib.sha256(package.raw).hexdigest(),
                "targets": {},
            }
            for name, frames in targets.items():
                score = score_phrase_target(frames, package, args.z)
                package_report["targets"][name] = score
                print(
                    f"phrase {kib:4d} KiB {name:12s} phrases={len(package.phrases)} "
                    f"literal/candidate/selected={score['literal_wire']}/{score['candidate_wire']}/"
                    f"{score['selected_wire']} covered={score['covered']}/{score['target_bytes']} "
                    f"ops phrase/add={score['phrase_ops']}/{score['add_ops']} "
                    f"wins={score['wins']}/{len(frames)} layouts={score['layout_wins']}",
                    file=sys.stderr,
                )
            report["phrase_models"].append(package_report)
        report["bpe_training"]["seconds"] = time.perf_counter() - phrase_started
    for kib in args.online_phrase_kib:
        for promotions in args.online_promotions:
            for minimum_count in args.online_minimum_count:
                for learning_rounds in args.online_rounds:
                    online_report = {
                        "budget_kib": kib,
                        "promotions_per_round": promotions,
                        "minimum_count": minimum_count,
                        "maximum_length": args.online_maximum_length,
                        "learning_rounds": learning_rounds,
                        "history_mib": args.online_history_mib,
                        "targets": {},
                    }
                    for name, frames in targets.items():
                        score = score_online_target(
                            frames, args.z, kib * 1024, promotions, minimum_count,
                            args.online_maximum_length, learning_rounds,
                            args.online_history_mib * 1024 * 1024,
                        )
                        online_report["targets"][name] = score
                        print(
                            f"online {kib:4d} KiB {name:12s} promote={promotions} "
                            f"min={minimum_count} rounds={learning_rounds} "
                            f"literal/candidate/selected={score['literal_wire']}/"
                            f"{score['candidate_wire']}/{score['selected_wire']} "
                            f"covered={score['covered']}/{score['target_bytes']} "
                            f"ops phrase/add={score['phrase_ops']}/{score['add_ops']} "
                            f"state={score['phrases']} phrases/{score['phrase_bytes']} bytes "
                            f"wins={score['wins']}/{len(frames)} layouts={score['layout_wins']}",
                            file=sys.stderr,
                        )
                    report["online_models"].append(online_report)
    report["seconds"] = time.perf_counter() - started
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.json:
        args.json.write_text(rendered, encoding="utf-8")
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
