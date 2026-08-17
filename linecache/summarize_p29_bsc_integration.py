#!/usr/bin/env python3
"""Validate retained bounded-BSC P29 runs and publish the exact gate report."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


OUTPUT_TOTAL = re.compile(r"TOTAL=(\d+)")
OUTPUT_EXACT = re.compile(r"byte-exact=OK\s+TUs=(\d+)")
OUTPUT_GROUP = re.compile(
    r"literal groups: tus_per_group=(\d+) groups=(\d+) workers=(\d+) "
    r"raw=(\d+) wire=(\d+) packed_header_bytes=(\d+) candidates=([^ ]+) "
    r"selected=\[zstd3=(\d+) bsc=(\d+) zstd10=(\d+)\] "
    r"encode_seconds=([0-9.]+) decode_seconds=([0-9.]+)"
)
ERROR_SPEED = re.compile(
    r"C-encode ([0-9.]+) GB/s \| F-decode ([0-9.]+) GB/s "
    r"=> pipelined min = ([0-9.]+) GB/s"
)
ERROR_RSS = re.compile(r"Maximum resident set size \(kbytes\): (\d+)")
ERROR_ELAPSED = re.compile(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)")


@dataclass(frozen=True)
class CorpusConfig:
    directory: str
    label: str
    expected_tus: int
    expected_groups: int


CONFIGS = (
    CorpusConfig("duckdb", "DuckDB", 689, 7),
    CorpusConfig("godot", "Godot", 2207, 20),
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def match_one(pattern: re.Pattern[str], text: str, description: str) -> re.Match[str]:
    matches = list(pattern.finditer(text))
    require(len(matches) == 1, f"expected one {description}, found {len(matches)}")
    return matches[0]


def elapsed_seconds(value: str) -> float:
    fields = [float(field) for field in value.split(":")]
    require(1 <= len(fields) <= 3, f"bad elapsed time: {value}")
    total = 0.0
    for field in fields:
        total = total * 60.0 + field
    return total


def read_curve(path: Path, expected_tus: int) -> tuple[list[dict[str, str]], str]:
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    require(len(rows) == expected_tus, f"{path}: expected {expected_tus} rows")
    previous_raw = 0
    previous_wire = 0
    for number, row in enumerate(rows, start=1):
        require(int(row["tu"]) == number, f"{path}: discontinuous TU row")
        require(row["exact"] == "true", f"{path}: non-exact TU {number}")
        cumulative_raw = int(row["cumulative_raw_bytes"])
        cumulative_wire = int(row["cumulative_wire_bytes"])
        require(
            cumulative_raw == previous_raw + int(row["raw_bytes"]),
            f"{path}: raw cumulative mismatch at TU {number}",
        )
        require(
            cumulative_wire == previous_wire + int(row["wire_bytes"]),
            f"{path}: wire cumulative mismatch at TU {number}",
        )
        previous_raw = cumulative_raw
        previous_wire = cumulative_wire
    return rows, sha256(path)


def read_components(path: Path, expected_tus: int) -> tuple[list[dict[str, str]], str]:
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    require(len(rows) == expected_tus, f"{path}: expected {expected_tus} component rows")
    previous_raw = 0
    previous_wire = 0
    for number, row in enumerate(rows, start=1):
        require(int(row["tu"]) == number, f"{path}: discontinuous component TU")
        require(row["exact"] == "true", f"{path}: non-exact component TU {number}")
        component_wire = sum(
            int(value)
            for name, value in row.items()
            if name.endswith("_wire_bytes")
            and name not in {"wire_bytes", "cumulative_wire_bytes"}
        )
        require(
            component_wire == int(row["wire_bytes"]),
            f"{path}: component sum mismatch at TU {number}",
        )
        cumulative_raw = int(row["cumulative_raw_bytes"])
        cumulative_wire = int(row["cumulative_wire_bytes"])
        require(
            cumulative_raw == previous_raw + int(row["raw_bytes"]),
            f"{path}: component raw cumulative mismatch at TU {number}",
        )
        require(
            cumulative_wire == previous_wire + int(row["wire_bytes"]),
            f"{path}: component wire cumulative mismatch at TU {number}",
        )
        previous_raw = cumulative_raw
        previous_wire = cumulative_wire
    return rows, sha256(path)


def reference_rows(path: Path) -> dict[str, dict[str, Any]]:
    data = json.loads(path.read_text())
    return {row["corpus"]: row for row in data["rows"]}


def parse_run(root: Path, config: CorpusConfig, repetition: int) -> dict[str, Any]:
    prefix = root / config.directory / f"g112-znver3-rep{repetition}"
    output_path = prefix.with_suffix(".out")
    error_path = prefix.with_suffix(".err")
    wire_path = prefix.with_suffix(".wire")
    curve_path = prefix.with_suffix(".curve.tsv")
    component_path = prefix.with_suffix(".components.tsv")
    for path in (output_path, error_path, wire_path, curve_path, component_path):
        require(path.is_file(), f"missing retained evidence: {path}")

    output = output_path.read_text()
    error = error_path.read_text()
    exact = match_one(OUTPUT_EXACT, output, "exact result")
    total = match_one(OUTPUT_TOTAL, output, "complete wire total")
    group = match_one(OUTPUT_GROUP, output, "literal-group result")
    speed = match_one(ERROR_SPEED, error, "split speed result")
    rss = match_one(ERROR_RSS, error, "maximum RSS result")
    elapsed = match_one(ERROR_ELAPSED, error, "elapsed result")

    curve, curve_hash = read_curve(curve_path, config.expected_tus)
    components, component_hash = read_components(component_path, config.expected_tus)
    require(int(exact.group(1)) == config.expected_tus, "output TU count differs")
    require(int(group.group(1)) == 112, "literal group boundary differs")
    require(int(group.group(2)) == config.expected_groups, "literal group count differs")
    require(int(group.group(6)) == 4, "packed header is not four bytes")
    require(group.group(7) == "zstd3,bsc", "speed row candidate set differs")
    require(int(group.group(8)) == 0 and int(group.group(10)) == 0, "unexpected selector")
    require(int(group.group(9)) == config.expected_groups, "not every group selected BSC")

    total_wire = int(total.group(1))
    literal_wire = int(group.group(5))
    require(wire_path.stat().st_size == literal_wire, "retained group-wire size differs")
    require(int(curve[-1]["cumulative_wire_bytes"]) == total_wire, "curve total differs")
    require(
        int(components[-1]["cumulative_wire_bytes"]) == total_wire,
        "component total differs",
    )
    require(
        int(curve[-1]["cumulative_raw_bytes"])
        == int(components[-1]["cumulative_raw_bytes"]),
        "curve raw totals differ",
    )

    return {
        "repetition": repetition,
        "tus": config.expected_tus,
        "raw_bytes": int(curve[-1]["cumulative_raw_bytes"]),
        "complete_wire_bytes": total_wire,
        "literal_raw_bytes": int(group.group(4)),
        "literal_wire_bytes": literal_wire,
        "groups": int(group.group(2)),
        "workers": int(group.group(3)),
        "group_encode_seconds": float(group.group(11)),
        "group_decode_seconds": float(group.group(12)),
        "c_encode_gbps": float(speed.group(1)),
        "f_decode_gbps": float(speed.group(2)),
        "pipeline_gbps": float(speed.group(3)),
        "max_rss_kib": int(rss.group(1)),
        "elapsed_seconds": elapsed_seconds(elapsed.group(1)),
        "tu100_wire_bytes": int(curve[99]["cumulative_wire_bytes"]),
        "tu200_wire_bytes": int(curve[199]["cumulative_wire_bytes"]),
        "wire_sha256": sha256(wire_path),
        "curve_sha256": curve_hash,
        "component_curve_sha256": component_hash,
        "stdout_sha256": sha256(output_path),
        "stderr_sha256": sha256(error_path),
    }


def fmt_delta(value: int) -> str:
    return f"{value:+,}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument(
        "--gate-json",
        type=Path,
        default=Path("linecache/ml-artifacts/compression-tournament-gates-p29-fixed16.json"),
    )
    parser.add_argument(
        "--output-md", type=Path, default=Path("linecache/P29-BSC-GROUP-INTEGRATION.md")
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        default=Path("linecache/ml-artifacts/p29-bsc-group-integration.json"),
    )
    parser.add_argument(
        "--output-tsv",
        type=Path,
        default=Path("linecache/ml-artifacts/p29-bsc-group-integration.tsv"),
    )
    args = parser.parse_args()

    references = reference_rows(args.gate_json)
    corpora: list[dict[str, Any]] = []
    for config in CONFIGS:
        runs = [parse_run(args.evidence_root, config, repetition) for repetition in (1, 2, 3)]
        invariant_fields = (
            "raw_bytes",
            "complete_wire_bytes",
            "literal_raw_bytes",
            "literal_wire_bytes",
            "groups",
            "tu100_wire_bytes",
            "tu200_wire_bytes",
            "wire_sha256",
            "curve_sha256",
            "component_curve_sha256",
        )
        for field in invariant_fields:
            require(len({run[field] for run in runs}) == 1, f"{config.label}: {field} varies")
        reference = references[config.directory]
        selected = runs[0]
        require(selected["raw_bytes"] == reference["raw_ii_bytes"], "reference raw differs")
        cold_delta = selected["complete_wire_bytes"] - reference["cold_110pct_gate_bytes"]
        tu100_delta = selected["tu100_wire_bytes"] - reference["tu100"]["z6long_bytes"]
        tu200_delta = selected["tu200_wire_bytes"] - reference["tu200"]["z6long_bytes"]
        min_c = min(run["c_encode_gbps"] for run in runs)
        min_f = min(run["f_decode_gbps"] for run in runs)
        require(cold_delta <= 0, f"{config.label}: cold gate failed")
        require(tu100_delta <= 0, f"{config.label}: TU100 gate failed")
        require(tu200_delta <= 0, f"{config.label}: TU200 gate failed")
        require(min_c >= 1.0 and min_f >= 1.0, f"{config.label}: speed gate failed")
        corpora.append(
            {
                "corpus": config.directory,
                "label": config.label,
                "tus": selected["tus"],
                "raw_bytes": selected["raw_bytes"],
                "whole_ii_z19_bytes": reference["z19long_ii_bytes"],
                "cold_gate_bytes": reference["cold_110pct_gate_bytes"],
                "complete_wire_bytes": selected["complete_wire_bytes"],
                "cold_delta_bytes": cold_delta,
                "literal_raw_bytes": selected["literal_raw_bytes"],
                "literal_wire_bytes": selected["literal_wire_bytes"],
                "groups": selected["groups"],
                "tu100_reference_bytes": reference["tu100"]["z6long_bytes"],
                "tu100_wire_bytes": selected["tu100_wire_bytes"],
                "tu100_delta_bytes": tu100_delta,
                "tu200_reference_bytes": reference["tu200"]["z6long_bytes"],
                "tu200_wire_bytes": selected["tu200_wire_bytes"],
                "tu200_delta_bytes": tu200_delta,
                "minimum_c_encode_gbps": min_c,
                "minimum_f_decode_gbps": min_f,
                "wire_sha256": selected["wire_sha256"],
                "runs": runs,
                "passes": {"cold": True, "tu100": True, "tu200": True, "speed": True},
            }
        )

    provenance = (args.evidence_root / "provenance.txt").read_text().splitlines()
    result = {
        "schema_version": 1,
        "scope": "complete exact P29 replay with fixed 112-TU precomputed literal groups",
        "group_tus": 112,
        "frame": "u32le: upper 3 bits codec, lower 29 bits payload length",
        "speed_candidates": ["zstd3", "libbsc-bwt-qlfc-adaptive"],
        "retained_decoder_tag": "zstd10",
        "all_checked_gates_pass": True,
        "corpora": corpora,
        "provenance": provenance,
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")

    fieldnames = [
        "corpus",
        "tus",
        "raw_bytes",
        "whole_ii_z19_bytes",
        "cold_gate_bytes",
        "complete_wire_bytes",
        "cold_delta_bytes",
        "tu100_reference_bytes",
        "tu100_wire_bytes",
        "tu100_delta_bytes",
        "tu200_reference_bytes",
        "tu200_wire_bytes",
        "tu200_delta_bytes",
        "minimum_c_encode_gbps",
        "minimum_f_decode_gbps",
        "wire_sha256",
        "exact",
    ]
    with args.output_tsv.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        for corpus in corpora:
            writer.writerow({**{name: corpus.get(name) for name in fieldnames}, "exact": True})

    lines = [
        "# Complete P29 bounded-BSC integration",
        "",
        "## Result",
        "",
        "A single fixed **112-TU** precompute boundary passes every currently binding gate on the two",
        "large corpora used to choose and stress the residual codec. These are complete P29",
        "encode/decode/accounting rows, not residual substitutions or projected totals.",
        "",
        "| corpus | complete wire | cold gate | cold delta | TU100 / z6-long | TU200 / z6-long | min C / F GB/s | result |",
        "|---|---:|---:|---:|---:|---:|---:|:---:|",
    ]
    for corpus in corpora:
        lines.append(
            f"| {corpus['label']} | {corpus['complete_wire_bytes']:,} | "
            f"{corpus['cold_gate_bytes']:,} | {fmt_delta(corpus['cold_delta_bytes'])} | "
            f"{corpus['tu100_wire_bytes']:,} / {corpus['tu100_reference_bytes']:,} | "
            f"{corpus['tu200_wire_bytes']:,} / {corpus['tu200_reference_bytes']:,} | "
            f"{corpus['minimum_c_encode_gbps']:.3f} / "
            f"{corpus['minimum_f_decode_gbps']:.3f} | **PASS** |"
        )
    lines.extend(
        [
            "",
        "The 112 boundary is the smallest measured clean common operating point between the earlier",
            "100-TU and 128-TU candidates: 100 misses DuckDB cold by 7,132 bytes; 128 charges",
            "material through TU256 at TU129 and misses Godot's TU200 reference. At 112, DuckDB is",
            "10,228 bytes inside its cold gate and Godot is 154,133 bytes inside its TU200 gate.",
            "",
            "This establishes the integrated method on DuckDB and Godot. It does **not** claim a",
            "fixed-16 closure; the same exact row still has to run across the remaining corpora.",
            "",
            "## Exact wire contract",
            "",
            "Each nonempty group is one independently decodable frame:",
            "",
            "```text",
            "u32 little-endian header",
            "  bits 31..29: codec (0=zstd-3, 1=libbsc BWT+adaptive QLFC, 2=zstd-10)",
            "  bits 28..0 : payload byte length",
            "payload[payload_length]",
            "```",
            "",
            "Libbsc payloads retain their self-describing 28-byte block headers and are capped at",
            "64 MiB raw per internal block. The speed-gated row evaluates zstd-3 and BSC. Zstd-10",
            "was removed from the timed candidate set because it selected zero groups at every",
            "measured 10-TU-or-larger DuckDB/Godot boundary and zero at 112; removing the losing",
            "trial changes no wire byte. Decoder tag 2 remains supported.",
            "",
            "The sender reads a complete per-TU length plan, forms consecutive groups of at most 112",
            "TUs, emits each group's full frame at its first charged TU, and records that exact frame",
            "in the component ledger. The receiver decodes only the selected frame and feeds its",
            "literal bytes into the ordinary P29 control-program decoder. The current encoder",
            "regenerates each TU's raw literal lane; a stale plan with different content necessarily",
            "fails final `.ii` comparison, while a length mismatch fails earlier.",
            "",
            "## Repetition evidence",
            "",
            "| corpus | rep | C GB/s | F GB/s | group encode / decode s | peak RSS MiB | elapsed s |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for corpus in corpora:
        for run in corpus["runs"]:
            lines.append(
                f"| {corpus['label']} | {run['repetition']} | "
                f"{run['c_encode_gbps']:.3f} | {run['f_decode_gbps']:.3f} | "
                f"{run['group_encode_seconds']:.3f} / {run['group_decode_seconds']:.3f} | "
                f"{run['max_rss_kib'] / 1024:.1f} | {run['elapsed_seconds']:.2f} |"
            )
    lines.extend(
        [
            "",
            "All three repetitions per corpus produced the same complete byte count, group-frame",
            "SHA-256, per-TU curve SHA-256, and component-ledger SHA-256. Every curve has exactly one",
            "row per TU; every row is exact; every component row sums to complete physical wire; and",
            "both cumulative ledgers close to the reported total.",
            "",
            "## Boundaries and limitations",
            "",
            "- This is bounded precompute, not zero-lookahead streaming. A group must be available",
            "  before its first TU can be reconstructed, so the method trades at most 112 TUs of",
            "  startup/lookahead for the measured compression.",
            "- Group encode/decode time is added serially to the split C/F proxy; no unimplemented",
            "  overlap credit is taken. The gate is therefore conservative for independent lanes.",
            "- The split result is the accepted research harness, not a preprocessor-pipe-to-compiler",
            "  deployment measurement. That end-to-end product measurement remains later work.",
            "- Peak RSS includes the full corpus, research dictionaries, both verification stores,",
            "  retained raw plan, decoded plan, and analysis ledgers. It is not a per-job cache target.",
            "- The current evidence covers DuckDB and Godot only. Fixed-16 breadth and the three",
            "  remaining structural cold misses remain open.",
            "",
            "## Reproduction",
            "",
            "The target build uses `-O3 -march=znver3`, libbsc 3.3.12 commit",
            "`baffa62c70b6ebbecc9af14ce550e965ea247680`, and the retained zstd 1.4.8 MT",
            "library. Runs are pinned with `taskset -c 0-15` on `tt-quietbox2` and use",
            "`OMP_NUM_THREADS=1`; group-worker counts are 7 for DuckDB and 16 for Godot.",
            "",
            "Machine-readable results: `ml-artifacts/p29-bsc-group-integration.json` and",
            "`ml-artifacts/p29-bsc-group-integration.tsv`.",
            "",
            "Retained run root: `/tmp/issue16-p29-bsc-integration-20260817` on",
            "`ttuser@tt-quietbox2`.",
        ]
    )
    args.output_md.write_text("\n".join(lines) + "\n")
    print(f"validated {len(CONFIGS) * 3} exact runs; all selected gates PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
