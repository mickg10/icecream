#!/usr/bin/env python3
"""Deterministic binding checks for the grouped GRZ2 research codec.

The suite expects the grz2g command-line interface used by the G0/G1/G2 runner.  It is
deliberately independent of the codec implementation and parses only the documented
STREAM/GROUP/END framing needed to locate exact group boundaries.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass


MAGIC_STREAM = 0x335A5247  # GRZ3
MAGIC_GROUP = 0x46505247   # GRPF
MAGIC_END = 0x444E4547     # GEND
STREAM_HEADER_BYTES = 72


class GateFailure(RuntimeError):
    pass


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_fixture(root: Path, name: str, tus: list[bytes]) -> tuple[Path, Path]:
    raw_path = root / f"{name}.ii"
    map_path = root / f"{name}.tu"
    offsets = [0]
    with raw_path.open("wb") as raw:
        for tu in tus:
            raw.write(tu)
            offsets.append(offsets[-1] + len(tu))
    with map_path.open("wb") as mapping:
        for offset in offsets:
            mapping.write(struct.pack("<Q", offset))
    return raw_path, map_path


def run(command: list[str], expect: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != expect:
        rendered = " ".join(command)
        raise GateFailure(
            f"unexpected status {result.returncode}, wanted {expect}: {rendered}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def run_must_fail(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode == 0:
        raise GateFailure(f"command unexpectedly succeeded: {' '.join(command)}")
    return result


class FrameReader:
    def __init__(self, data: bytes, offset: int = 0):
        self.data = data
        self.offset = offset

    def take(self, size: int) -> bytes:
        end = self.offset + size
        if end > len(self.data):
            raise GateFailure("container ends inside frame metadata")
        value = self.data[self.offset:end]
        self.offset = end
        return value

    def u8(self) -> int:
        return self.take(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]


@dataclass(frozen=True)
class GroupFrame:
    index: int
    start: int
    raw_size: int
    history_base: int
    history_extent: int
    tu_lengths: tuple[int, ...]
    frame_start: int
    frame_end: int


@dataclass(frozen=True)
class Container:
    groups: tuple[GroupFrame, ...]
    end_frame_start: int
    end_offset: int
    end_raw: int
    end_groups: int
    end_matches: int
    end_digest: int


def parse_container(path: Path) -> Container:
    data = path.read_bytes()
    if len(data) < STREAM_HEADER_BYTES:
        raise GateFailure("container is shorter than the static header")
    header = FrameReader(data)
    if header.u32() != MAGIC_STREAM or header.u32() != 3:
        raise GateFailure("unexpected stream header")
    header.take(STREAM_HEADER_BYTES - header.offset)
    reader = FrameReader(data, STREAM_HEADER_BYTES)
    groups: list[GroupFrame] = []

    while True:
        frame_start = reader.offset
        magic = reader.u32()
        if magic == MAGIC_END:
            end_raw = reader.u64()
            end_groups = reader.u64()
            end_matches = reader.u64()
            end_digest = reader.u64()
            if reader.offset != len(data):
                raise GateFailure("bytes follow END frame")
            return Container(tuple(groups), frame_start, reader.offset, end_raw,
                             end_groups, end_matches, end_digest)
        if magic != MAGIC_GROUP:
            raise GateFailure(f"unknown frame magic at byte {frame_start}")

        index = reader.u64()
        start = reader.u64()
        raw_size = reader.u64()
        history_base = reader.u64()
        history_extent = reader.u64()
        ntu = reader.u32()
        tu_lengths = tuple(reader.u64() for _ in range(ntu))
        reader.take(5)  # offset mode plus four backend IDs
        raw_sizes = tuple(reader.u64() for _ in range(4))
        compressed_sizes = tuple(reader.u64() for _ in range(4))
        literal_blocks = reader.u32()
        literal_compressed = 0
        for _ in range(literal_blocks):
            literal_compressed += reader.u64()
            reader.u8()
        reader.u64()  # group digest
        if literal_compressed != compressed_sizes[1]:
            raise GateFailure("literal block table does not sum to c1")
        reader.take(sum(compressed_sizes))
        if sum(tu_lengths) != raw_size:
            raise GateFailure("TU lengths do not sum to group raw bytes")
        if raw_sizes[1] > raw_size:
            raise GateFailure("ADD stream exceeds group raw bytes")
        groups.append(GroupFrame(index, start, raw_size, history_base, history_extent,
                                 tu_lengths, frame_start, reader.offset))


def codec_options(*, history_mb: int = 5, group_raw_mb: int = 4,
                  group_add_mb: int = 2, group_tus: int = 3,
                  retry_every: int = 0, curve: Path | None = None) -> list[str]:
    options = [
        "-m", "g2", "-K", "64", "-s", "0", "-t", "20",
        "-l", "2", "-k", "1", "-b", "1", "-j", "2",
        "--gtu", str(group_tus), "--graw", str(group_raw_mb),
        "--gadd", str(group_add_mb), "--hist", str(history_mb),
    ]
    if retry_every:
        options += ["--retry-test", str(retry_every)]
    if curve is not None:
        options += ["--curve", str(curve)]
    return options


def encode(codec: Path, raw: Path, tu_map: Path, output: Path,
           options: list[str]) -> subprocess.CompletedProcess[str]:
    return run([str(codec), "enc", str(raw), str(output), "-u", str(tu_map), *options])


def decode(codec: Path, container: Path, output: Path,
           prefix: bool = False) -> subprocess.CompletedProcess[str]:
    mode = "decprefix" if prefix else "dec"
    return run([str(codec), mode, str(container), str(output), "-j", "1"])


def assert_exact(a: Path, b: Path, label: str) -> None:
    if a.stat().st_size != b.stat().st_size or sha256(a) != sha256(b):
        raise GateFailure(f"{label}: byte-exact replay failed")


def mutate_u64(source: Path, output: Path, offset: int, delta: int = 1) -> None:
    data = bytearray(source.read_bytes())
    if offset < 0 or offset + 8 > len(data):
        raise GateFailure(f"mutation offset {offset} is outside {source}")
    original = struct.unpack_from("<Q", data, offset)[0]
    struct.pack_into("<Q", data, offset, (original + delta) & ((1 << 64) - 1))
    output.write_bytes(data)


def require_decode_rejection(codec: Path, wire: Path, root: Path, label: str,
                             *, prefix: bool = False) -> None:
    mode = "decprefix" if prefix else "dec"
    run_must_fail([str(codec), mode, str(wire), str(root / f"reject-{label}.out"),
                   "-j", "1"])


def basic_and_prefix_gate(codec: Path, root: Path) -> None:
    common = (
        b"# 1 /usr/include/vector\n"
        + b"template<class T> inline T probe(T x){ return x + 123456; }\n"
    ) * 8000
    tus: list[bytes] = []
    for i in range(12):
        part = common + (f"// TU {i:03d}\nint unique_{i}={i};\n".encode() * 2000)
        if i % 3 == 0:
            part += b"A" * 70000 + b"BCDE" * 30000
        tus.append(part)

    full_raw, full_map = write_fixture(root, "basic-full", tus)
    prefix_raw, prefix_map = write_fixture(root, "basic-prefix", tus[:6])
    full_wire = root / "basic-full.grz"
    prefix_wire = root / "basic-prefix.grz"
    options = codec_options()
    full_result = encode(codec, full_raw, full_map, full_wire, options)
    prefix_result = encode(codec, prefix_raw, prefix_map, prefix_wire, options)
    if "retry_fail=0" not in full_result.stderr or "retry_fail=0" not in prefix_result.stderr:
        raise GateFailure("basic encode did not report a clean transaction check")

    decoded = root / "basic-full.out"
    decode(codec, full_wire, decoded)
    assert_exact(full_raw, decoded, "basic full")

    full_frames = parse_container(full_wire)
    prefix_frames = parse_container(prefix_wire)
    if len(full_frames.groups) != 4 or len(prefix_frames.groups) != 2:
        raise GateFailure("unexpected basic group count")
    prefix_end = prefix_frames.groups[-1].frame_end
    if full_wire.read_bytes()[:prefix_end] != prefix_wire.read_bytes()[:prefix_end]:
        raise GateFailure("encode(P) differs from prefix(encode(P || S))")

    exact_cut = root / "basic-cut.grz"
    exact_cut.write_bytes(full_wire.read_bytes()[:prefix_end])
    run_must_fail([str(codec), "dec", str(exact_cut), str(root / "strict-cut.out"),
                   "-j", "1"])
    prefix_out = root / "prefix-cut.out"
    decode(codec, exact_cut, prefix_out, prefix=True)
    assert_exact(prefix_raw, prefix_out, "exact prefix cut")

    plus_one = root / "basic-cut-plus-one.grz"
    plus_one.write_bytes(full_wire.read_bytes()[:prefix_end + 1])
    run_must_fail([str(codec), "decprefix", str(plus_one),
                   str(root / "plus-one.out"), "-j", "1"])

    inside = root / "basic-cut-inside.grz"
    inside.write_bytes(full_wire.read_bytes()[:full_frames.groups[2].frame_end - 1])
    run_must_fail([str(codec), "decprefix", str(inside),
                   str(root / "inside.out"), "-j", "1"])

    # A complete container is also a valid prefix.  Once its END frame is present, however,
    # prefix mode must close the same totals and reject trailing bytes just like full mode.
    prefix_full_out = root / "prefix-full.out"
    decode(codec, full_wire, prefix_full_out, prefix=True)
    assert_exact(full_raw, prefix_full_out, "complete container in prefix mode")
    bad_prefix_end = root / "basic-bad-prefix-end.grz"
    mutate_u64(full_wire, bad_prefix_end, full_frames.end_frame_start + 4)
    require_decode_rejection(codec, bad_prefix_end, root, "bad-prefix-end", prefix=True)
    trailing_after_end = root / "basic-trailing-after-end.grz"
    trailing_after_end.write_bytes(full_wire.read_bytes() + b"X")
    require_decode_rejection(codec, trailing_after_end, root, "trailing-after-end",
                             prefix=True)

    # Bind every state-bearing group field that the writer emits.  Silently ignoring any of
    # these permits a wire state with no corresponding encoder transition.
    first = full_frames.groups[0]
    mutations = [
        ("group-index", first.frame_start + 4),
        ("history-extent", first.frame_start + 36),
        ("tu-length-sum", first.frame_start + 48),
    ]
    # After magic/index/start/raw/history-base/history-extent/ntu/TU lengths come five
    # one-byte modes, four raw u64 sizes, then c0/c1/c2/c3.  Alter c1 while leaving the
    # literal-block table untouched; the decoder must require both accounts to agree.
    after_tus = first.frame_start + 48 + 8 * len(first.tu_lengths)
    mutations.append(("literal-compressed-total", after_tus + 45))
    for label, offset in mutations:
        changed = root / f"basic-bad-{label}.grz"
        mutate_u64(full_wire, changed, offset)
        require_decode_rejection(codec, changed, root, label)

    bad_end_matches = root / "basic-bad-end-matches.grz"
    mutate_u64(full_wire, bad_end_matches, full_frames.end_frame_start + 20)
    require_decode_rejection(codec, bad_end_matches, root, "end-match-total")
    print("PASS exact replay, prefix identity, strict cuts, and frame/state closure")


def zero_anchor_gate(codec: Path, root: Path) -> None:
    rng = random.Random(918273)
    # Exactly two K-byte copies make the only useful source start at absolute byte zero.
    block = bytes(rng.randrange(0, 256) for _ in range(64))
    raw, mapping = write_fixture(root, "zero-anchor", [block + block])
    wire = root / "zero-anchor.grz"
    encode(codec, raw, mapping, wire,
           codec_options(history_mb=1, group_raw_mb=1, group_add_mb=1, group_tus=1))
    parsed = parse_container(wire)
    if parsed.end_matches == 0:
        raise GateFailure("source byte zero was not used by any COPY")
    output = root / "zero-anchor.out"
    decode(codec, wire, output)
    assert_exact(raw, output, "byte-zero/same-group")
    print("PASS byte-zero anchor and same-group anchor visibility")


def rollback_gate(codec: Path, root: Path) -> None:
    rng = random.Random(778899)
    tus = [bytes(rng.randrange(0, 256) for _ in range(700_000)) for _ in range(4)]
    raw, mapping = write_fixture(root, "rollback", tus)
    wire = root / "rollback.grz"
    curve = root / "rollback.tsv"
    result = encode(codec, raw, mapping, wire,
                    codec_options(history_mb=2, group_raw_mb=4, group_add_mb=1,
                                  group_tus=4, retry_every=1, curve=curve))
    if "retry_fail=0" not in result.stderr:
        raise GateFailure("transaction replay check failed")
    if "\tadd\n" not in curve.read_text():
        raise GateFailure("fixture did not exercise ADD-cap rollback")
    output = root / "rollback.out"
    decode(codec, wire, output)
    assert_exact(raw, output, "rollback")
    print("PASS rollback from provisional anchors and deterministic retry")


def ring_wrap_gate(codec: Path, root: Path) -> None:
    base = (b"template<class T> T ring_probe(T x){return x+1234567;}\n" * 12500)[:700000]
    tus = [base[:-64] + f"//{i:060d}\n".encode()[:64] for i in range(220)]
    raw, mapping = write_fixture(root, "ring-wrap", tus)
    wire = root / "ring-wrap.grz"
    encode(codec, raw, mapping, wire,
           codec_options(history_mb=1, group_raw_mb=2, group_add_mb=4, group_tus=3))
    output = root / "ring-wrap.out"
    decode(codec, wire, output)
    assert_exact(raw, output, "repeated ring wrap")
    print("PASS repeated physical ring wrap and composable group/whole digests")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("codec", type=Path)
    parser.add_argument("--workdir", type=Path)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--skip-wrap", action="store_true")
    args = parser.parse_args()
    codec = args.codec.resolve()
    if not codec.is_file() or not os.access(codec, os.X_OK):
        raise GateFailure(f"codec is not executable: {codec}")

    temporary = args.workdir is None
    root = args.workdir.resolve() if args.workdir else Path(tempfile.mkdtemp(prefix="grz2-binding-"))
    root.mkdir(parents=True, exist_ok=True)
    try:
        basic_and_prefix_gate(codec, root)
        zero_anchor_gate(codec, root)
        rollback_gate(codec, root)
        if not args.skip_wrap:
            ring_wrap_gate(codec, root)
        print(f"ALL GRZ2 BINDING CHECKS PASSED: {root}")
        return 0
    finally:
        if temporary and not args.keep:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GateFailure as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
