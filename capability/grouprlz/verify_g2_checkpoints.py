#!/usr/bin/env python3
"""Verify GRZ2 checkpoint charging, lookahead, and exact prefix replay.

The curve charges each complete group frame to that group's first TU.  This is the
conservative W(P,N) accounting used at TU100/TU200: if any of the first N TUs belongs
to a group, the complete frame is charged, and the resulting future-TU/raw-byte wait is
reported explicitly.  Bytes physically emitted when exactly N source TUs have arrived
are reported separately and are never substituted for the conservative charge.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path


STREAM_HEADER_BYTES = 72
END_FRAME_BYTES = 36
MAGIC_STREAM = 0x335A5247
MAGIC_GROUP = 0x46505247
MAGIC_END = 0x444E4547


class VerificationFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Group:
    index: int
    tu_lo: int
    tu_hi: int
    raw_bytes: int
    add_bytes: int
    wire_bytes: int
    history_base: int
    history_extent: int
    closed_by: str


def load_tu_map(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) < 16 or len(data) % 8:
        raise VerificationFailure(f"invalid TU map length: {path}")
    offsets = list(struct.unpack(f"<{len(data) // 8}Q", data))
    if offsets[0] != 0 or any(a > b for a, b in zip(offsets, offsets[1:])):
        raise VerificationFailure("TU offsets are not monotonic from zero")
    return offsets


def load_curve(path: Path, offsets: list[int]) -> list[Group]:
    groups: list[Group] = []
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            group = Group(
                index=int(row["group"]),
                tu_lo=int(row["tu_lo"]),
                tu_hi=int(row["tu_hi"]),
                raw_bytes=int(row["out_bytes"]),
                add_bytes=int(row["add_bytes"]),
                wire_bytes=int(row["comp_bytes"]),
                history_base=int(row["hist_base"]),
                history_extent=int(row["hist_extent"]),
                closed_by=row["closed_by"],
            )
            groups.append(group)

    if not groups:
        raise VerificationFailure("curve contains no groups")
    expected_lo = 0
    for expected_index, group in enumerate(groups):
        if group.index != expected_index:
            raise VerificationFailure("group ordinals are not contiguous")
        if group.tu_lo != expected_lo or group.tu_hi <= group.tu_lo:
            raise VerificationFailure("group TU ranges are not contiguous")
        if group.tu_hi >= len(offsets):
            raise VerificationFailure("group TU range exceeds the TU map")
        if offsets[group.tu_hi] - offsets[group.tu_lo] != group.raw_bytes:
            raise VerificationFailure("curve raw byte count disagrees with TU map")
        if group.add_bytes > group.raw_bytes or group.wire_bytes <= 0:
            raise VerificationFailure("invalid group ADD or wire byte count")
        expected_lo = group.tu_hi
    if expected_lo != len(offsets) - 1:
        raise VerificationFailure("curve does not cover every TU")
    return groups


def load_z6(path: Path, corpus: str) -> dict[int, tuple[int, int]]:
    result: dict[int, tuple[int, int]] = {}
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            if row["corpus"] == corpus:
                result[int(row["N"])] = (int(row["prefix_raw"]),
                                          int(row["z6_long31_bytes"]))
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def compare_raw_prefix(raw: Path, decoded: Path, size: int) -> str:
    if decoded.stat().st_size != size:
        raise VerificationFailure(
            f"decoded size {decoded.stat().st_size} does not equal prefix size {size}"
        )
    raw_digest = hashlib.sha256()
    decoded_digest = hashlib.sha256()
    remaining = size
    with raw.open("rb") as source, decoded.open("rb") as replay:
        while remaining:
            amount = min(1 << 20, remaining)
            left = source.read(amount)
            right = replay.read(amount)
            if left != right:
                raise VerificationFailure("decoded bytes differ from raw prefix")
            raw_digest.update(left)
            decoded_digest.update(right)
            remaining -= amount
        if replay.read(1):
            raise VerificationFailure("decoded output has trailing bytes")
    if raw_digest.digest() != decoded_digest.digest():
        raise VerificationFailure("prefix hashes differ after byte comparison")
    return raw_digest.hexdigest()


def run(command: list[str], stdout_path: Path, stderr_path: Path) -> int:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    stdout_path.write_text(result.stdout)
    stderr_path.write_text(result.stderr)
    return result.returncode


def validate_container(wire: Path, groups: list[Group]) -> bytes:
    data = wire.read_bytes()
    expected = STREAM_HEADER_BYTES + sum(group.wire_bytes for group in groups) + END_FRAME_BYTES
    if len(data) != expected:
        raise VerificationFailure(f"wire size {len(data)} does not equal curve account {expected}")
    if struct.unpack_from("<I", data, 0)[0] != MAGIC_STREAM:
        raise VerificationFailure("bad stream magic")
    cursor = STREAM_HEADER_BYTES
    for group in groups:
        if struct.unpack_from("<I", data, cursor)[0] != MAGIC_GROUP:
            raise VerificationFailure(f"bad group magic at group {group.index}")
        cursor += group.wire_bytes
    if struct.unpack_from("<I", data, cursor)[0] != MAGIC_END:
        raise VerificationFailure("bad END frame magic")
    return data


def write_group_ledger(path: Path, corpus: str, groups: list[Group],
                       offsets: list[int]) -> None:
    fields = [
        "corpus", "group", "first_tu_zero_based", "last_tu_zero_based",
        "material_available_through_tu_count", "group_tus",
        "lookahead_tus_after_first", "raw_bytes", "raw_lookahead_after_first",
        "add_bytes", "bytes_charged_at_first_tu", "cumulative_wire_after_frame",
        "history_base", "history_extent", "closed_by",
    ]
    cumulative = STREAM_HEADER_BYTES
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        for group in groups:
            cumulative += group.wire_bytes
            writer.writerow({
                "corpus": corpus,
                "group": group.index,
                "first_tu_zero_based": group.tu_lo,
                "last_tu_zero_based": group.tu_hi - 1,
                "material_available_through_tu_count": group.tu_hi,
                "group_tus": group.tu_hi - group.tu_lo,
                "lookahead_tus_after_first": group.tu_hi - group.tu_lo - 1,
                "raw_bytes": group.raw_bytes,
                "raw_lookahead_after_first": offsets[group.tu_hi] - offsets[group.tu_lo + 1],
                "add_bytes": group.add_bytes,
                "bytes_charged_at_first_tu": group.wire_bytes,
                "cumulative_wire_after_frame": cumulative,
                "history_base": group.history_base,
                "history_extent": group.history_extent,
                "closed_by": group.closed_by,
            })


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--codec", required=True, type=Path)
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--tu-map", required=True, type=Path)
    parser.add_argument("--wire", required=True, type=Path)
    parser.add_argument("--curve", required=True, type=Path)
    parser.add_argument("--z6-ledger", required=True, type=Path)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--workdir", required=True, type=Path)
    parser.add_argument("--checkpoints", nargs="+", type=int, default=[100, 200])
    args = parser.parse_args()

    for path in (args.codec, args.raw, args.tu_map, args.wire, args.curve,
                 args.z6_ledger):
        if not path.is_file():
            raise VerificationFailure(f"missing input: {path}")
    args.workdir.mkdir(parents=True, exist_ok=True)

    offsets = load_tu_map(args.tu_map)
    if args.raw.stat().st_size != offsets[-1]:
        raise VerificationFailure("raw input size disagrees with TU map")
    groups = load_curve(args.curve, offsets)
    wire_data = validate_container(args.wire, groups)
    z6 = load_z6(args.z6_ledger, args.corpus)
    write_group_ledger(args.workdir / f"{args.corpus}.group-charges.tsv",
                       args.corpus, groups, offsets)

    fields = [
        "corpus", "N", "raw_at_N", "charged_W", "physically_emitted_at_N",
        "physical_material_through_tu_count", "material_available_through_tu_count",
        "lookahead_tus", "raw_lookahead_bytes", "z6_long31_bytes", "W_over_z6",
        "gate", "prefix_exact", "strict_decode_rejects_cut", "cut_sha256",
        "decoded_prefix_sha256",
    ]
    rows: list[dict[str, object]] = []
    for checkpoint in args.checkpoints:
        if checkpoint <= 0 or checkpoint >= len(offsets):
            raise VerificationFailure(f"checkpoint outside corpus: {checkpoint}")
        if checkpoint not in z6:
            raise VerificationFailure(f"missing z6 row for TU{checkpoint}")
        raw_at_n = offsets[checkpoint]
        if z6[checkpoint][0] != raw_at_n:
            raise VerificationFailure(f"z6 raw count mismatch at TU{checkpoint}")

        charged_groups = [group for group in groups if group.tu_lo < checkpoint]
        emitted_groups = [group for group in groups if group.tu_hi <= checkpoint]
        coverage = charged_groups[-1].tu_hi
        physical_coverage = emitted_groups[-1].tu_hi if emitted_groups else 0
        charged = STREAM_HEADER_BYTES + sum(group.wire_bytes for group in charged_groups)
        emitted = STREAM_HEADER_BYTES + sum(group.wire_bytes for group in emitted_groups)
        raw_lookahead = offsets[coverage] - raw_at_n

        cut = args.workdir / f"{args.corpus}.tu{checkpoint}.cut.grz"
        cut.write_bytes(wire_data[:charged])
        decoded = args.workdir / f"{args.corpus}.tu{checkpoint}.decoded.ii"
        prefix_stdout = args.workdir / f"{args.corpus}.tu{checkpoint}.decprefix.stdout"
        prefix_stderr = args.workdir / f"{args.corpus}.tu{checkpoint}.decprefix.stderr"
        status = run([str(args.codec), "decprefix", str(cut), str(decoded), "-j", "1"],
                     prefix_stdout, prefix_stderr)
        if status != 0:
            raise VerificationFailure(f"prefix decode failed at TU{checkpoint}")
        prefix_hash = compare_raw_prefix(args.raw, decoded, offsets[coverage])

        strict_stdout = args.workdir / f"{args.corpus}.tu{checkpoint}.strict.stdout"
        strict_stderr = args.workdir / f"{args.corpus}.tu{checkpoint}.strict.stderr"
        strict_status = run([str(args.codec), "dec", str(cut), "/dev/null", "-j", "1"],
                            strict_stdout, strict_stderr)
        if strict_status == 0:
            raise VerificationFailure(f"strict decode accepted an END-less cut at TU{checkpoint}")
        decoded.unlink()

        z6_bytes = z6[checkpoint][1]
        rows.append({
            "corpus": args.corpus,
            "N": checkpoint,
            "raw_at_N": raw_at_n,
            "charged_W": charged,
            "physically_emitted_at_N": emitted,
            "physical_material_through_tu_count": physical_coverage,
            "material_available_through_tu_count": coverage,
            "lookahead_tus": coverage - checkpoint,
            "raw_lookahead_bytes": raw_lookahead,
            "z6_long31_bytes": z6_bytes,
            "W_over_z6": f"{charged / z6_bytes:.4f}",
            "gate": "PASS" if charged <= z6_bytes else "FAIL",
            "prefix_exact": "YES",
            "strict_decode_rejects_cut": "YES",
            "cut_sha256": sha256(cut),
            "decoded_prefix_sha256": prefix_hash,
        })

    output = args.workdir / f"{args.corpus}.checkpoints.tsv"
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print("\t".join(str(row[field]) for field in fields))
    if any(row["gate"] != "PASS" for row in rows):
        raise VerificationFailure("one or more checkpoint byte gates failed")
    print(f"ALL CHECKPOINT CUTS PASSED: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerificationFailure as error:
        print(f"FAIL: {error}")
        raise SystemExit(1)
