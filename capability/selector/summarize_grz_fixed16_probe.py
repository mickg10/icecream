#!/usr/bin/env python3
"""Audit and summarize the current-GRZ fixed-16 selector probes.

The probe runner writes its result row only after an exact complete replay.  This
summarizer independently checks the retained wire, TU map, curves, manifests,
tooling digests, endpoint reports, and source ledger.  It then emits only causal
features available at the frozen ``min(112, complete TUs)`` decision boundary.

Process timings are retained as diagnostics.  They are not the isolated
common-input rate measurement and are deliberately excluded from selector
features.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import struct
from typing import Any, Iterable


CHUNK = 8 * 1024 * 1024
ENC_RE = re.compile(
    r"ENC .* mode=G(?P<mode>\d+) raw=(?P<raw>\d+) out=(?P<wire>\d+) "
    r"groups=(?P<groups>\d+).* C=(?P<bps>[0-9.]+) B/s .*"
    r"retry_fail=(?P<retry>\d+) oversize_tu=(?P<oversize>\d+)"
)
DEC_RE = re.compile(
    r"DEC .* groups=(?P<groups>\d+) bytes=(?P<raw>\d+).* "
    r"F=(?P<bps>[0-9.]+) B/s .* \[FULL,VERIFIED\]"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def read_key_values(path: Path, separator: str = "=") -> dict[str, str]:
    result: dict[str, str] = {}
    for number, line in enumerate(path.read_text().splitlines(), start=1):
        fields = line.split(separator, 1)
        require(len(fields) == 2 and all(fields), f"bad field at {path}:{number}")
        key, value = fields
        require(key not in result, f"duplicate field {key!r}: {path}")
        result[key] = value
    return result


def read_hashes(path: Path) -> dict[Path, str]:
    result: dict[Path, str] = {}
    for number, line in enumerate(path.read_text().splitlines(), start=1):
        fields = line.split(maxsplit=1)
        require(len(fields) == 2, f"bad digest at {path}:{number}")
        digest, name = fields
        require(re.fullmatch(r"[0-9a-f]{64}", digest) is not None, f"bad SHA-256: {path}")
        artifact = Path(name)
        require(artifact not in result, f"duplicate digest path: {artifact}")
        result[artifact] = digest
    return result


def one_match(pattern: re.Pattern[str], text: str, description: str) -> re.Match[str]:
    matches = list(pattern.finditer(text))
    require(len(matches) == 1, f"expected one {description}, found {len(matches)}")
    return matches[0]


def finite_ratio(numerator: int, denominator: int, description: str) -> float:
    require(denominator > 0, f"zero denominator for {description}")
    value = numerator / denominator
    require(math.isfinite(value), f"non-finite {description}")
    return value


def verify_tu_map(path: Path, sizes: list[int]) -> None:
    raw = path.read_bytes()
    require(len(raw) == 8 * (len(sizes) + 1), f"TU-map extent differs: {path}")
    offsets = list(struct.unpack(f"<{len(sizes) + 1}Q", raw))
    expected = [0]
    for size in sizes:
        expected.append(expected[-1] + size)
    require(offsets == expected, f"TU-map offsets differ: {path}")


def verify_tooling(root: Path) -> tuple[str, str]:
    hashes = read_hashes(root / "tooling.sha256")
    require(len(hashes) == 4, "expected runner, GRZ binary, GRZ source, and ledger digests")
    for path, expected in hashes.items():
        require(path.is_file(), f"missing tooling input: {path}")
        require(sha256_file(path) == expected, f"tooling digest differs: {path}")
    sources = [(path, digest) for path, digest in hashes.items() if path.name.endswith(".cpp")]
    binaries = [(path, digest) for path, digest in hashes.items() if path.name == "grz2g-selector"]
    require(len(sources) == 1 and len(binaries) == 1, "GRZ source/binary provenance differs")
    return sources[0][1], binaries[0][1]


def verify_exact_hashes(cell: Path) -> None:
    hashes = read_hashes(cell / "exact.sha256")
    by_name = {path.name: digest for path, digest in hashes.items()}
    require(len(by_name) == 5, f"wrong exact-digest count: {cell}")
    require(by_name["probe.ii"] == by_name["replay.ii"], f"source/replay digest differs: {cell}")
    for name in ("probe.tu", "probe.grz", "curve.tsv"):
        path = cell / name
        require(path.is_file(), f"missing retained probe artifact: {path}")
        require(sha256_file(path) == by_name[name], f"probe artifact digest differs: {path}")


def sum_field(rows: Iterable[dict[str, str]], field: str) -> int:
    return sum(int(row[field]) for row in rows)


def parse_cell(
    cell: Path,
    ledger: dict[str, str],
    group_tus: int,
    source_sha256: str,
    binary_sha256: str,
) -> dict[str, Any]:
    measurements = read_tsv(cell / "measurement.tsv")
    require(len(measurements) == 1, f"expected one measurement row: {cell}")
    measured = measurements[0]
    identity = (ledger["id"], ledger["name"])
    require((measured["id"], measured["name"]) == identity, f"cell identity differs: {cell}")
    require(measured["exact"] == "true", f"runner did not report exact: {identity}")
    for field in ("total_tus", "total_raw_bytes", "manifest_sha256"):
        ledger_field = {"total_tus": "tu_count", "total_raw_bytes": "raw_bytes"}.get(field, field)
        require(measured[field] == ledger[ledger_field], f"ledger field differs for {identity}: {field}")

    total_tus = int(ledger["tu_count"])
    probe_tus = int(measured["probe_tus"])
    probe_raw = int(measured["probe_raw_bytes"])
    probe_wire = int(measured["probe_wire_bytes"])
    require(probe_tus == min(group_tus, total_tus), f"probe boundary differs: {identity}")
    require((cell / "probe.grz").stat().st_size == probe_wire, f"wire size differs: {identity}")

    source_manifest = Path(ledger["manifest_path"])
    require(source_manifest.is_file(), f"missing source manifest: {source_manifest}")
    require(sha256_file(source_manifest) == ledger["manifest_sha256"], f"manifest changed: {identity}")
    source_paths = source_manifest.read_text().splitlines()
    probe_paths = (cell / "manifest.probe.txt").read_text().splitlines()
    require(len(source_paths) == total_tus, f"complete manifest extent differs: {identity}")
    require(probe_paths == source_paths[:probe_tus], f"probe manifest is not a prefix: {identity}")
    require(len(probe_paths) == len(set(probe_paths)), f"duplicate probe paths: {identity}")
    paths = [Path(name) for name in probe_paths]
    require(all(path.is_file() for path in paths), f"missing probe TU: {identity}")
    sizes = [path.stat().st_size for path in paths]
    require(sum(sizes) == probe_raw, f"probe raw extent differs: {identity}")
    verify_tu_map(cell / "probe.tu", sizes)
    verify_exact_hashes(cell)

    curve = read_tsv(cell / "curve.tsv")
    require(bool(curve), f"empty GRZ curve: {identity}")
    require([int(row["group"]) for row in curve] == list(range(len(curve))), f"group sequence differs: {identity}")
    tu_cursor = 0
    for row in curve:
        require(int(row["tu_lo"]) == tu_cursor, f"non-contiguous curve: {identity}")
        tu_hi = int(row["tu_hi"])
        require(tu_cursor < tu_hi <= probe_tus, f"bad curve TU extent: {identity}")
        tu_cursor = tu_hi
    require(tu_cursor == probe_tus, f"curve does not reach boundary: {identity}")
    require(sum_field(curve, "out_bytes") == probe_raw, f"curve raw total differs: {identity}")
    compressed_frames = sum_field(curve, "comp_bytes")
    framing_bytes = probe_wire - compressed_frames
    require(framing_bytes > 0, f"missing GRZ stream framing: {identity}")

    enc = one_match(ENC_RE, (cell / "encode.stderr").read_text(), "GRZ encode endpoint")
    dec = one_match(DEC_RE, (cell / "decode.stderr").read_text(), "GRZ decode endpoint")
    require(int(enc["mode"]) == 2, f"wrong GRZ mode: {identity}")
    require(int(enc["raw"]) == probe_raw and int(dec["raw"]) == probe_raw, f"endpoint raw differs: {identity}")
    require(int(enc["wire"]) == probe_wire, f"endpoint wire differs: {identity}")
    require(int(enc["groups"]) == len(curve) == int(dec["groups"]), f"group count differs: {identity}")
    require(int(enc["retry"]) == 0, f"GRZ retry check failed: {identity}")

    samples = sum_field(curve, "anchor_samples")
    occupied = sum_field(curve, "anchor_occupied")
    usable = sum_field(curve, "anchor_usable")
    collisions = sum_field(curve, "anchor_collisions")
    matches = sum_field(curve, "anchor_matches")
    require(0 <= matches <= usable <= occupied <= samples, f"anchor counters differ: {identity}")
    require(matches + collisions == usable, f"usable anchor accounting differs: {identity}")
    close_reasons = sorted({row["closed_by"] for row in curve})

    return {
        "id": ledger["id"],
        "name": ledger["name"],
        "total_tus": total_tus,
        "total_raw_bytes": int(ledger["raw_bytes"]),
        "probe_tus": probe_tus,
        "probe_raw_bytes": probe_raw,
        "probe_wire_bytes": probe_wire,
        "probe_group_count": len(curve),
        "probe_framing_bytes": framing_bytes,
        "probe_add_bytes": sum_field(curve, "add_bytes"),
        "probe_add_bpr": finite_ratio(sum_field(curve, "add_bytes"), probe_raw, "ADD/raw"),
        "probe_wire_bpr": finite_ratio(probe_wire, probe_raw, "wire/raw"),
        "probe_anchor_samples": samples,
        "probe_anchor_matches": matches,
        "probe_anchor_sample_per_raw": finite_ratio(samples, probe_raw, "samples/raw"),
        "probe_anchor_occupied_fraction": finite_ratio(occupied, samples, "occupied/samples"),
        "probe_anchor_usable_fraction": finite_ratio(usable, samples, "usable/samples"),
        "probe_anchor_match_fraction": finite_ratio(matches, usable, "matches/usable"),
        "probe_anchor_collision_fraction": finite_ratio(collisions, usable, "collisions/usable"),
        "probe_close_reasons": ",".join(close_reasons),
        "diagnostic_encode_bps": int(float(enc["bps"])),
        "diagnostic_decode_bps": int(float(dec["bps"])),
        "manifest_sha256": ledger["manifest_sha256"],
        "grz_source_sha256": source_sha256,
        "grz_binary_sha256": binary_sha256,
        "exact": True,
    }


def markdown(rows: list[dict[str, Any]], summary: dict[str, Any]) -> str:
    lines = [
        "# Current-GRZ fixed-16 selector probes",
        "",
        f"- Exact cells: **{summary['exact_cells']}/{summary['cells']}**.",
        f"- Frozen boundary: first **min({summary['group_tus']}, complete TUs)**.",
        f"- Probe raw bytes: **{summary['probe_raw_bytes']:,} B**.",
        f"- Probe wire bytes: **{summary['probe_wire_bytes']:,} B**.",
        "",
        "| corpus | probe TUs | raw MB | GRZ MB | ADD/raw | anchor match | groups | close |",
        "|---|---:|---:|---:|---:|---:|---:|:---|",
    ]
    for row in rows:
        lines.append(
            f"| {row['name']} | {row['probe_tus']} | {row['probe_raw_bytes']/1e6:.3f} | "
            f"{row['probe_wire_bytes']/1e6:.3f} | {row['probe_add_bpr']:.4f} | "
            f"{row['probe_anchor_match_fraction']:.4f} | {row['probe_group_count']} | "
            f"{row['probe_close_reasons']} |"
        )
    lines.extend(
        [
            "",
            "The endpoint rates above are deliberately retained only in the TSV as diagnostics;",
            "the selector features contain no measured wall-clock input.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--expected-cells", default=16, type=int)
    args = parser.parse_args()
    root = args.run_root.resolve()

    require((root / "fixed16-ledger.tsv").read_bytes() == (root / "fixed16-ledger.final.tsv").read_bytes(), "fixed-16 ledger changed during run")
    ledger_rows = read_tsv(root / "fixed16-ledger.tsv")
    require(len(ledger_rows) == args.expected_cells, "fixed-16 ledger count differs")
    config = read_key_values(root / "config")
    require(int(config["expected_cells"]) == args.expected_cells, "configured cell count differs")
    group_tus = int(config["group_tus"])
    require(group_tus > 0, "group_tus must be positive")
    meta = read_key_values(root / "run.meta")
    require("started_utc" in meta and "completed_utc" in meta, "run is not complete")
    source_sha256, binary_sha256 = verify_tooling(root)

    ledger = {row["id"]: row for row in ledger_rows}
    require(len(ledger) == len(ledger_rows), "duplicate fixed-16 ledger IDs")
    paths = sorted((root / "cells").glob("*/measurement.tsv"))
    require(len(paths) == args.expected_cells, "fixed-16 result count differs")
    result_ids = {path.parent.name for path in paths}
    require(result_ids == set(ledger), "fixed-16 result IDs differ from ledger")
    rows = [parse_cell(path.parent, ledger[path.parent.name], group_tus, source_sha256, binary_sha256) for path in paths]
    order = {row["id"]: index for index, row in enumerate(ledger_rows)}
    rows.sort(key=lambda row: order[row["id"]])

    summary = {
        "schema": "grz-fixed16-selector-probe-v1",
        "cells": len(rows),
        "group_tus": group_tus,
        "probe_raw_bytes": sum(row["probe_raw_bytes"] for row in rows),
        "probe_wire_bytes": sum(row["probe_wire_bytes"] for row in rows),
        "exact_cells": sum(row["exact"] for row in rows),
        "ledger_sha256": sha256_file(root / "fixed16-ledger.tsv"),
        "grz_source_sha256": source_sha256,
        "grz_binary_sha256": binary_sha256,
        "timing_scope": "diagnostic retained probe process",
    }
    with (root / "grz-fixed16-probe-measurements.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, delimiter="\t", lineterminator="\n", fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (root / "grz-fixed16-probe-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    (root / "GRZ-FIXED16-SELECTOR-PROBES.md").write_text(markdown(rows, summary))
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
