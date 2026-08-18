#!/usr/bin/env python3
"""Bind a complete P29 run's first group to a suffix-blind standalone run."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct


STREAMS = ("control", "literal", "array-control", "array-values")
HEADER_BYTES = 4
PAYLOAD_MASK = (1 << 29) - 1


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_tsv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        rows = list(reader)
        if reader.fieldnames is None:
            raise RuntimeError(f"missing TSV header: {path}")
        return reader.fieldnames, rows


def read_lengths(prefix: Path, expected_rows: int) -> list[tuple[int, ...]]:
    raw = prefix.with_suffix(".lengths.raw").read_bytes()
    stride = 4 * len(STREAMS)
    if len(raw) != expected_rows * stride:
        raise RuntimeError(
            f"length table {prefix} has {len(raw)} bytes; expected {expected_rows * stride}"
        )
    return [
        struct.unpack_from(f"<{len(STREAMS)}I", raw, row * stride)
        for row in range(expected_rows)
    ]


def parse_literal_frames(path: Path) -> list[bytes]:
    wire = path.read_bytes()
    frames: list[bytes] = []
    offset = 0
    while offset < len(wire):
        if len(wire) - offset < HEADER_BYTES:
            raise RuntimeError(f"truncated literal-group header: {path}")
        header = struct.unpack_from("<I", wire, offset)[0]
        kind = header >> 29
        payload = header & PAYLOAD_MASK
        if kind > 2:
            raise RuntimeError(f"unknown literal-group kind {kind}: {path}")
        end = offset + HEADER_BYTES + payload
        if end > len(wire):
            raise RuntimeError(f"truncated literal-group payload: {path}")
        frames.append(wire[offset:end])
        offset = end
    return frames


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--full-curve", type=Path, required=True)
    parser.add_argument("--prefix-curve", type=Path, required=True)
    parser.add_argument("--full-components", type=Path, required=True)
    parser.add_argument("--prefix-components", type=Path, required=True)
    parser.add_argument("--full-literal-wire", type=Path, required=True)
    parser.add_argument("--prefix-literal-wire", type=Path, required=True)
    parser.add_argument("--full-plan-prefix", type=Path, required=True)
    parser.add_argument("--prefix-plan-prefix", type=Path, required=True)
    parser.add_argument("--prefix-tus", type=int, required=True)
    parser.add_argument("--group-tus", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.prefix_tus <= 0 or args.group_tus <= 0:
        raise RuntimeError("prefix and group TU counts must be positive")

    full_curve_header, full_curve = read_tsv(args.full_curve)
    prefix_curve_header, prefix_curve = read_tsv(args.prefix_curve)
    full_component_header, full_components = read_tsv(args.full_components)
    prefix_component_header, prefix_components = read_tsv(args.prefix_components)
    if full_curve_header != prefix_curve_header:
        raise RuntimeError("curve schemas differ")
    if full_component_header != prefix_component_header:
        raise RuntimeError("component schemas differ")
    if len(prefix_curve) != args.prefix_tus or len(prefix_components) != args.prefix_tus:
        raise RuntimeError("standalone ledger row count differs from prefix TU count")
    if len(full_curve) < args.prefix_tus or len(full_components) < args.prefix_tus:
        raise RuntimeError("complete ledger is shorter than requested prefix")
    if any(row.get("exact") != "true" for row in full_curve + prefix_curve):
        raise RuntimeError("a curve row is not exact")
    if any(row.get("exact") != "true" for row in full_components + prefix_components):
        raise RuntimeError("a component row is not exact")

    curve_identical = full_curve[: args.prefix_tus] == prefix_curve
    components_identical = full_components[: args.prefix_tus] == prefix_components
    if not curve_identical:
        raise RuntimeError("complete and standalone prefix curves differ")
    if not components_identical:
        raise RuntimeError("complete and standalone component prefixes differ")

    full_lengths = read_lengths(args.full_plan_prefix, len(full_curve))
    prefix_lengths = read_lengths(args.prefix_plan_prefix, len(prefix_curve))
    if full_lengths[: args.prefix_tus] != prefix_lengths:
        raise RuntimeError("complete and standalone raw-plane length prefixes differ")
    plan_hashes: dict[str, dict[str, str | int]] = {}
    for stream_index, name in enumerate(STREAMS):
        full_path = args.full_plan_prefix.with_suffix(f".{name}.raw")
        prefix_path = args.prefix_plan_prefix.with_suffix(f".{name}.raw")
        prefix_bytes = sum(row[stream_index] for row in prefix_lengths)
        full_data = full_path.read_bytes()
        prefix_data = prefix_path.read_bytes()
        if len(prefix_data) != prefix_bytes:
            raise RuntimeError(f"standalone {name} plane extent differs from its length table")
        if full_data[:prefix_bytes] != prefix_data:
            raise RuntimeError(f"complete and standalone {name} raw-plane prefixes differ")
        plan_hashes[name] = {
            "prefix_bytes": prefix_bytes,
            "prefix_sha256": hashlib.sha256(prefix_data).hexdigest(),
        }

    full_frames = parse_literal_frames(args.full_literal_wire)
    prefix_frames = parse_literal_frames(args.prefix_literal_wire)
    expected_frames = (args.prefix_tus + args.group_tus - 1) // args.group_tus
    if len(prefix_frames) != expected_frames:
        raise RuntimeError(
            f"standalone prefix produced {len(prefix_frames)} literal frames; "
            f"expected {expected_frames}"
        )
    if full_frames[:expected_frames] != prefix_frames:
        raise RuntimeError("complete and standalone literal-group frame prefixes differ")

    for row in prefix_components:
        component_sum = sum(
            int(value)
            for name, value in row.items()
            if name.endswith("_wire_bytes")
            and name not in {"wire_bytes", "cumulative_wire_bytes"}
        )
        if component_sum != int(row["wire_bytes"]):
            raise RuntimeError(f"component row {row['tu']} does not sum to complete wire")

    result = {
        "schema": "p29-prefix-identity-v1",
        "prefix_tus": args.prefix_tus,
        "group_tus": args.group_tus,
        "full_tus": len(full_curve),
        "prefix_mode": (
            "suffix-blind" if args.prefix_tus < len(full_curve) else "complete-program"
        ),
        "curve_identical": curve_identical,
        "components_identical": components_identical,
        "prefix_wire_bytes": int(prefix_curve[-1]["cumulative_wire_bytes"]),
        "prefix_raw_bytes": int(prefix_curve[-1]["cumulative_raw_bytes"]),
        "literal_frames": len(prefix_frames),
        "literal_frame_bytes": sum(map(len, prefix_frames)),
        "literal_frame_sha256": hashlib.sha256(b"".join(prefix_frames)).hexdigest(),
        "plan_prefixes": plan_hashes,
        "artifacts": {
            str(path): sha256(path)
            for path in (
                args.full_curve,
                args.prefix_curve,
                args.full_components,
                args.prefix_components,
                args.full_literal_wire,
                args.prefix_literal_wire,
            )
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
