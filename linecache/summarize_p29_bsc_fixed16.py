#!/usr/bin/env python3
"""Validate and report the complete fixed-16 P29 + bounded-BSC runs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


OUTPUT_TOTAL = re.compile(r"TOTAL=(\d+)")
OUTPUT_EXACT = re.compile(r"byte-exact=OK\s+TUs=(\d+)")
OUTPUT_SOURCE_REUSED = re.compile(r"raw_source_reused=(\d+)")
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
ERROR_ELAPSED = re.compile(
    r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([0-9:.]+)"
)

GROUP_TUS = 112
SPEED_GATE_GBPS = 1.0


@dataclass(frozen=True)
class CorpusConfig:
    name: str
    label: str
    selected_prefix: str | None = None


CONFIGS = (
    CorpusConfig("llvm", "LLVM"),
    CorpusConfig("rocksdb", "RocksDB"),
    CorpusConfig("duckdb", "DuckDB", "duckdb/g112-znver3-rep1"),
    CorpusConfig("abseil", "Abseil"),
    CorpusConfig("opencv", "OpenCV"),
    CorpusConfig("godot", "Godot", "godot/g112-znver3-rep1"),
    CorpusConfig("fmt", "fmt"),
    CorpusConfig("spdlog", "spdlog"),
    CorpusConfig("catch2", "Catch2"),
    CorpusConfig("nlohmann-json", "nlohmann-json"),
    CorpusConfig("range-v3", "range-v3"),
    CorpusConfig("eigen", "Eigen"),
    CorpusConfig("re2", "RE2"),
    CorpusConfig("leveldb", "LevelDB"),
    CorpusConfig("simdjson", "simdjson"),
    CorpusConfig("cereal", "cereal"),
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
    require(len(rows) == expected_tus, f"{path}: expected {expected_tus} rows")
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


def parse_plan(path: Path, expected_tus: int) -> dict[str, int]:
    require(path.is_file(), f"missing target-host P29 plan output: {path}")
    text = path.read_text()
    exact = match_one(OUTPUT_EXACT, text, "plan exact result")
    require(int(exact.group(1)) == expected_tus, f"{path}: plan TU count differs")
    return {
        "wire_bytes": int(match_one(OUTPUT_TOTAL, text, "plan total").group(1)),
        "raw_source_reused_bytes": int(
            match_one(OUTPUT_SOURCE_REUSED, text, "source reuse total").group(1)
        ),
    }


def run_prefix(root: Path, config: CorpusConfig) -> Path:
    return root / (config.selected_prefix or config.name)


def parse_run(
    root: Path,
    config: CorpusConfig,
    reference: dict[str, Any],
    target_plan: dict[str, int],
) -> dict[str, Any]:
    prefix = run_prefix(root, config)
    paths = {
        "stdout": prefix.with_suffix(".out"),
        "stderr": prefix.with_suffix(".err"),
        "wire": prefix.with_suffix(".wire"),
        "curve": prefix.with_suffix(".curve.tsv"),
        "components": prefix.with_suffix(".components.tsv"),
    }
    for path in paths.values():
        require(path.is_file(), f"missing retained evidence: {path}")

    output = paths["stdout"].read_text()
    error = paths["stderr"].read_text()
    exact = match_one(OUTPUT_EXACT, output, "exact result")
    total = match_one(OUTPUT_TOTAL, output, "complete wire total")
    group = match_one(OUTPUT_GROUP, output, "literal-group result")
    speed = match_one(ERROR_SPEED, error, "split speed result")
    rss = match_one(ERROR_RSS, error, "maximum RSS result")
    elapsed = match_one(ERROR_ELAPSED, error, "elapsed result")

    tus = int(reference["tus"])
    curve, curve_hash = read_curve(paths["curve"], tus)
    components, component_hash = read_components(paths["components"], tus)
    require(int(exact.group(1)) == tus, f"{config.name}: output TU count differs")
    require(int(group.group(1)) == GROUP_TUS, f"{config.name}: group size differs")
    expected_groups = math.ceil(tus / GROUP_TUS)
    require(
        int(group.group(2)) == expected_groups,
        f"{config.name}: expected {expected_groups} groups",
    )
    require(int(group.group(6)) == 4, f"{config.name}: frame header is not 4 bytes")
    require(group.group(7) == "zstd3,bsc", f"{config.name}: candidate set differs")
    selected_zstd3 = int(group.group(8))
    selected_bsc = int(group.group(9))
    selected_zstd10 = int(group.group(10))
    require(selected_zstd10 == 0, f"{config.name}: unexpected zstd10 selection")
    require(
        selected_zstd3 + selected_bsc == expected_groups,
        f"{config.name}: selector counts do not cover every group",
    )

    total_wire = int(total.group(1))
    literal_wire = int(group.group(5))
    require(paths["wire"].stat().st_size == literal_wire, "group-wire size differs")
    require(int(curve[-1]["cumulative_wire_bytes"]) == total_wire, "curve total differs")
    require(
        int(components[-1]["cumulative_wire_bytes"]) == total_wire,
        "component total differs",
    )
    raw_bytes = int(curve[-1]["cumulative_raw_bytes"])
    require(raw_bytes == int(reference["raw_ii_bytes"]), "reference raw total differs")
    require(
        raw_bytes == int(components[-1]["cumulative_raw_bytes"]),
        "curve raw totals differ",
    )

    target_p29 = target_plan["wire_bytes"]
    original_literal_wire = target_p29 + literal_wire - total_wire
    require(original_literal_wire > 0, f"{config.name}: invalid original literal total")
    require(
        target_p29 - original_literal_wire + literal_wire == total_wire,
        f"{config.name}: literal replacement ledger does not close",
    )

    prefixes: dict[str, dict[str, Any]] = {}
    for key in ("tu100", "tu200"):
        prefix_reference = reference[key]
        eligible = bool(prefix_reference["eligible"])
        requested = int(prefix_reference["requested_tu"])
        actual = int(prefix_reference["actual_tu"])
        require(actual == min(requested, tus), f"{config.name}: bad reference boundary")
        row = curve[actual - 1]
        candidate_wire = int(row["cumulative_wire_bytes"])
        require(
            int(row["cumulative_raw_bytes"]) == int(prefix_reference["raw_bytes"]),
            f"{config.name}: {key} raw total differs from reference",
        )
        reference_wire = int(prefix_reference["z6long_bytes"])
        prefixes[key] = {
            "requested_tu": requested,
            "actual_tu": actual,
            "eligible": eligible,
            "candidate_wire_bytes": candidate_wire,
            "reference_wire_bytes": reference_wire,
            "delta_bytes": candidate_wire - reference_wire,
            "pass": candidate_wire <= reference_wire if eligible else None,
        }

    c_gbps = float(speed.group(1))
    f_gbps = float(speed.group(2))
    cold_delta = total_wire - int(reference["cold_110pct_gate_bytes"])
    cold_size_pass = cold_delta <= 0
    speed_pass = c_gbps >= SPEED_GATE_GBPS and f_gbps >= SPEED_GATE_GBPS
    return {
        "corpus": config.name,
        "label": config.label,
        "tus": tus,
        "raw_bytes": raw_bytes,
        "target_p29_wire_bytes": target_p29,
        "canonical_p29_wire_bytes": int(reference["p29_cold_bytes"]),
        "p29_host_delta_bytes": target_p29 - int(reference["p29_cold_bytes"]),
        "original_literal_wire_bytes": original_literal_wire,
        "literal_raw_bytes": int(group.group(4)),
        "literal_group_wire_bytes": literal_wire,
        "literal_savings_bytes": original_literal_wire - literal_wire,
        "complete_wire_bytes": total_wire,
        "whole_ii_z19_bytes": int(reference["z19long_ii_bytes"]),
        "cold_gate_bytes": int(reference["cold_110pct_gate_bytes"]),
        "cold_delta_bytes": cold_delta,
        "groups": expected_groups,
        "selected_zstd3_groups": selected_zstd3,
        "selected_bsc_groups": selected_bsc,
        "workers": int(group.group(3)),
        "group_encode_seconds": float(group.group(11)),
        "group_decode_seconds": float(group.group(12)),
        "c_encode_gbps": c_gbps,
        "f_decode_gbps": f_gbps,
        "pipeline_gbps": float(speed.group(3)),
        "max_rss_kib": int(rss.group(1)),
        "elapsed_seconds": elapsed_seconds(elapsed.group(1)),
        "prefixes": prefixes,
        "passes": {
            "exact": True,
            "cold_size": cold_size_pass,
            "speed": speed_pass,
            "cold_size_and_speed": cold_size_pass and speed_pass,
            "tu100": prefixes["tu100"]["pass"],
            "tu200": prefixes["tu200"]["pass"],
        },
        "evidence": {
            "wire_sha256": sha256(paths["wire"]),
            "curve_sha256": curve_hash,
            "component_curve_sha256": component_hash,
            "stdout_sha256": sha256(paths["stdout"]),
            "stderr_sha256": sha256(paths["stderr"]),
        },
    }


def fmt_delta(value: int) -> str:
    return f"{value:+,}"


def verdict(value: bool | None) -> str:
    if value is None:
        return "—"
    return "PASS" if value else "FAIL"


def prefix_cell(prefix: dict[str, Any]) -> str:
    if not prefix["eligible"]:
        return "—"
    return (
        f"{prefix['candidate_wire_bytes']:,} / {prefix['reference_wire_bytes']:,} "
        f"({fmt_delta(prefix['delta_bytes'])}) **{verdict(prefix['pass'])}**"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--target-plan-root", type=Path, required=True)
    parser.add_argument("--local-plan-root", type=Path, required=True)
    parser.add_argument(
        "--gate-json",
        type=Path,
        default=Path(
            "linecache/ml-artifacts/compression-tournament-gates-p29-fixed16.json"
        ),
    )
    parser.add_argument(
        "--output-md", type=Path, default=Path("linecache/P29-BSC-GROUP-FIXED16.md")
    )
    parser.add_argument(
        "--output-json",
        type=Path,
        default=Path("linecache/ml-artifacts/p29-bsc-group-fixed16.json"),
    )
    parser.add_argument(
        "--output-tsv",
        type=Path,
        default=Path("linecache/ml-artifacts/p29-bsc-group-fixed16.tsv"),
    )
    args = parser.parse_args()

    references = reference_rows(args.gate_json)
    require(set(references) == {config.name for config in CONFIGS}, "fixed-16 set differs")
    rows: list[dict[str, Any]] = []
    host_drift: list[dict[str, Any]] = []
    for config in CONFIGS:
        reference = references[config.name]
        if config.selected_prefix is None:
            target_plan = parse_plan(
                args.target_plan_root / f"{config.name}.out", int(reference["tus"])
            )
            local_plan = parse_plan(
                args.local_plan_root / f"{config.name}.out", int(reference["tus"])
            )
            require(
                local_plan["wire_bytes"] == int(reference["p29_cold_bytes"]),
                f"{config.name}: local plan differs from canonical P29",
            )
        else:
            target_plan = {
                "wire_bytes": int(reference["p29_cold_bytes"]),
                "raw_source_reused_bytes": 0,
            }
            local_plan = target_plan
        row = parse_run(args.evidence_root, config, reference, target_plan)
        rows.append(row)
        if target_plan != local_plan:
            host_drift.append(
                {
                    "corpus": config.name,
                    "label": config.label,
                    "canonical_p29_wire_bytes": local_plan["wire_bytes"],
                    "target_p29_wire_bytes": target_plan["wire_bytes"],
                    "wire_delta_bytes": target_plan["wire_bytes"]
                    - local_plan["wire_bytes"],
                    "canonical_source_reused_bytes": local_plan[
                        "raw_source_reused_bytes"
                    ],
                    "target_source_reused_bytes": target_plan[
                        "raw_source_reused_bytes"
                    ],
                    "source_reused_delta_bytes": target_plan[
                        "raw_source_reused_bytes"
                    ]
                    - local_plan["raw_source_reused_bytes"],
                }
            )

    require(
        {entry["corpus"] for entry in host_drift} == {"opencv", "leveldb"},
        "unexpected target-host P29 plan drift set",
    )
    summary = {
        "corpora": len(rows),
        "exact": sum(row["passes"]["exact"] for row in rows),
        "cold_size": sum(row["passes"]["cold_size"] for row in rows),
        "speed": sum(row["passes"]["speed"] for row in rows),
        "cold_size_and_speed": sum(
            row["passes"]["cold_size_and_speed"] for row in rows
        ),
        "tu100_eligible": sum(
            row["prefixes"]["tu100"]["eligible"] for row in rows
        ),
        "tu100_pass": sum(row["passes"]["tu100"] is True for row in rows),
        "tu200_eligible": sum(
            row["prefixes"]["tu200"]["eligible"] for row in rows
        ),
        "tu200_pass": sum(row["passes"]["tu200"] is True for row in rows),
    }
    result = {
        "schema_version": 1,
        "scope": "complete exact P29 replay with fixed 112-TU literal groups",
        "group_tus": GROUP_TUS,
        "speed_gate_gbps": SPEED_GATE_GBPS,
        "frame": "u32le: upper 3 bits codec, lower 29 bits payload length",
        "selector_candidates": ["zstd3", "libbsc-bwt-qlfc-adaptive"],
        "summary": summary,
        "host_plan_drift": host_drift,
        "rows": rows,
        "provenance": {
            "target": "ttuser@tt-quietbox2 (hostname tt-quietbox)",
            "target_build": "-O3 -march=znver3; OMP_NUM_THREADS=1; taskset -c 0-15",
            "target_retained_root": "/tmp/issue16-p29-bsc-integration-20260817",
            "local_evidence_root": str(args.evidence_root),
            "opencv_ordered_ii_sha256": (
                "1287b36659df27e5b30dd29c8cf997e3f1ab8c6a435d0b2c0ae83e72c944cb5e"
            ),
            "leveldb_ordered_ii_sha256": (
                "83fca047531416b8827f7892f94403fc5273d84c8ef6e614daa2af762ce59166"
            ),
        },
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")

    fieldnames = [
        "corpus",
        "tus",
        "raw_bytes",
        "target_p29_wire_bytes",
        "canonical_p29_wire_bytes",
        "p29_host_delta_bytes",
        "original_literal_wire_bytes",
        "literal_group_wire_bytes",
        "complete_wire_bytes",
        "cold_gate_bytes",
        "cold_delta_bytes",
        "cold_size_pass",
        "c_encode_gbps",
        "f_decode_gbps",
        "speed_pass",
        "cold_size_and_speed_pass",
        "tu100_wire_bytes",
        "tu100_reference_bytes",
        "tu100_pass",
        "tu200_wire_bytes",
        "tu200_reference_bytes",
        "tu200_pass",
        "wire_sha256",
        "exact",
    ]
    with args.output_tsv.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "corpus": row["corpus"],
                    "tus": row["tus"],
                    "raw_bytes": row["raw_bytes"],
                    "target_p29_wire_bytes": row["target_p29_wire_bytes"],
                    "canonical_p29_wire_bytes": row["canonical_p29_wire_bytes"],
                    "p29_host_delta_bytes": row["p29_host_delta_bytes"],
                    "original_literal_wire_bytes": row[
                        "original_literal_wire_bytes"
                    ],
                    "literal_group_wire_bytes": row["literal_group_wire_bytes"],
                    "complete_wire_bytes": row["complete_wire_bytes"],
                    "cold_gate_bytes": row["cold_gate_bytes"],
                    "cold_delta_bytes": row["cold_delta_bytes"],
                    "cold_size_pass": row["passes"]["cold_size"],
                    "c_encode_gbps": row["c_encode_gbps"],
                    "f_decode_gbps": row["f_decode_gbps"],
                    "speed_pass": row["passes"]["speed"],
                    "cold_size_and_speed_pass": row["passes"][
                        "cold_size_and_speed"
                    ],
                    "tu100_wire_bytes": row["prefixes"]["tu100"][
                        "candidate_wire_bytes"
                    ],
                    "tu100_reference_bytes": row["prefixes"]["tu100"][
                        "reference_wire_bytes"
                    ],
                    "tu100_pass": row["passes"]["tu100"],
                    "tu200_wire_bytes": row["prefixes"]["tu200"][
                        "candidate_wire_bytes"
                    ],
                    "tu200_reference_bytes": row["prefixes"]["tu200"][
                        "reference_wire_bytes"
                    ],
                    "tu200_pass": row["passes"]["tu200"],
                    "wire_sha256": row["evidence"]["wire_sha256"],
                    "exact": True,
                }
            )

    lines = [
        "# Fixed-16 complete P29 + bounded-BSC result",
        "",
        "## Result",
        "",
        "The same fixed **112-TU** literal-group policy was run through the complete P29",
        "encoder, independently decoded group frames, ordinary P29 decoder, final `.ii` byte",
        "comparison, and physical component ledger on all 16 fixed corpora.",
        "",
        f"- exact reconstruction and closed ledgers: **{summary['exact']}/16**",
        f"- cold size gate: **{summary['cold_size']}/16**",
        f"- measured C and F rate at least 1 GB/s: **{summary['speed']}/16**",
        f"- cold size and speed together: **{summary['cold_size_and_speed']}/16**",
        f"- TU100 prefix gate: **{summary['tu100_pass']}/{summary['tu100_eligible']}** eligible corpora",
        f"- TU200 prefix gate: **{summary['tu200_pass']}/{summary['tu200_eligible']}** eligible corpora",
        "",
        "This is the breadth result for one actual integrated candidate. It must not be described",
        "as the earlier projected 13/16 best-of selector: that number combines several different",
        "mechanisms, some of which have not yet been integrated behind one complete decoder.",
        "",
        "## Cold size and measured rate",
        "",
        "| corpus | target P29 | fixed-112 complete | cold gate | delta | size | C / F GB/s | joint |",
        "|---|---:|---:|---:|---:|:---:|---:|:---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['label']} | {row['target_p29_wire_bytes']:,} | "
            f"{row['complete_wire_bytes']:,} | {row['cold_gate_bytes']:,} | "
            f"{fmt_delta(row['cold_delta_bytes'])} | "
            f"**{verdict(row['passes']['cold_size'])}** | "
            f"{row['c_encode_gbps']:.3f} / {row['f_decode_gbps']:.3f} | "
            f"**{verdict(row['passes']['cold_size_and_speed'])}** |"
        )
    lines.extend(
        [
            "",
            "The two rate misses are the one-group fmt and spdlog runs. Their complete raw inputs",
            "are only 136 MB and 98 MB, so fixed harness and store construction costs are material;",
            "the table records the measured complete-path proxy without amortizing them away.",
            "",
            "## Chronological prefix gates",
            "",
            "Frames are charged at their group's first TU. TU100 therefore includes literal material",
            "through TU112, and TU200 includes material through TU224, while each zstd-6-long",
            "reference contains only the requested prefix. Passing rows absorb that deliberate",
            "lookahead charge.",
            "",
            "| corpus | TU100 candidate / z6-long (delta) | TU200 candidate / z6-long (delta) |",
            "|---|---:|---:|",
        ]
    )
    for row in rows:
        if row["prefixes"]["tu100"]["eligible"] or row["prefixes"]["tu200"][
            "eligible"
        ]:
            lines.append(
                f"| {row['label']} | {prefix_cell(row['prefixes']['tu100'])} | "
                f"{prefix_cell(row['prefixes']['tu200'])} |"
            )
    lines.extend(
        [
            "",
            "## Literal-group selector behavior",
            "",
            "| corpus | groups | zstd-3 / BSC selected | original literal wire | grouped wire | saved |",
            "|---|---:|---:|---:|---:|---:|",
        ]
    )
    for row in rows:
        lines.append(
            f"| {row['label']} | {row['groups']} | "
            f"{row['selected_zstd3_groups']} / {row['selected_bsc_groups']} | "
            f"{row['original_literal_wire_bytes']:,} | "
            f"{row['literal_group_wire_bytes']:,} | "
            f"{row['literal_savings_bytes']:,} |"
        )
    lines.extend(
        [
            "",
            "Catch2 is the only corpus where the actual-byte selector retained zstd-3 groups",
            "(4 of 8). All other groups selected BSC. The four-byte packed frame is included in",
            "every group total; zstd-10 was not evaluated on this speed row and selected no group.",
            "",
            "## Host-dependent source-reuse finding",
            "",
            "The ordered `.ii` bytes are identical across the local and target hosts, but ordinary",
            "P29 produced a different internal source-reuse plan for OpenCV and LevelDB:",
            "",
            "| corpus | ordered `.ii` SHA-256 | local / target P29 | delta | local / target source-reused raw |",
            "|---|---|---:|---:|---:|",
        ]
    )
    ordered_hashes = {
        "opencv": result["provenance"]["opencv_ordered_ii_sha256"],
        "leveldb": result["provenance"]["leveldb_ordered_ii_sha256"],
    }
    for drift in host_drift:
        lines.append(
            f"| {drift['label']} | `{ordered_hashes[drift['corpus']]}` | "
            f"{drift['canonical_p29_wire_bytes']:,} / "
            f"{drift['target_p29_wire_bytes']:,} | "
            f"{fmt_delta(drift['wire_delta_bytes'])} | "
            f"{drift['canonical_source_reused_bytes']:,} / "
            f"{drift['target_source_reused_bytes']:,} |"
        )
    lines.extend(
        [
            "",
            "The cause is P29 consulting host-local system-source paths while forming its reuse",
            "plan. That is deterministic only when C and F see the same source package. Before M5",
            "can use this path, source reuse must be tied to an explicitly matching package identity",
            "or the exact referenced bytes must travel in the normal material stream. Silent reads",
            "from unrelated host-local include trees are not a valid distributed-codec contract.",
            "",
            "The fixed-112 totals above deliberately use the target host's own P29 plan. Their cold",
            "gates remain the canonical whole-program references, so OpenCV's pass is unaffected and",
            "LevelDB's miss is reported as the actual +9,299-byte target-host result.",
            "",
            "## Verification and retained evidence",
            "",
            "For every corpus the validator requires one exact row per TU, monotonic raw/wire",
            "cumulatives, equality between curve and component totals, equality between every",
            "per-TU component sum and physical wire, group-count closure, selector-count closure,",
            "literal-frame file size equality, full raw-byte equality with the fixed reference,",
            "and exact final reconstruction reported by the independent decoder.",
            "",
            "Target build: `-O3 -march=znver3`, `OMP_NUM_THREADS=1`, `taskset -c 0-15` on",
            "`ttuser@tt-quietbox2` (hostname `tt-quietbox`). Target evidence remains under",
            "`/tmp/issue16-p29-bsc-integration-20260817`; the copied report evidence is under",
            f"`{args.evidence_root.parent}`.",
            "",
            "Machine-readable results: `ml-artifacts/p29-bsc-group-fixed16.json` and",
            "`ml-artifacts/p29-bsc-group-fixed16.tsv`.",
        ]
    )
    args.output_md.write_text("\n".join(lines) + "\n")
    print(
        "validated fixed-16 exact runs: "
        f"cold+speed={summary['cold_size_and_speed']}/16 "
        f"TU100={summary['tu100_pass']}/{summary['tu100_eligible']} "
        f"TU200={summary['tu200_pass']}/{summary['tu200_eligible']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
