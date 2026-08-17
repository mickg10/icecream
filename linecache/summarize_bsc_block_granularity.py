#!/usr/bin/env python3
"""Validate and render the exact BSC residual TU-group experiment."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import defaultdict
from pathlib import Path
from typing import Any


INTEGER_FIELDS = (
    "tus",
    "groups",
    "nonempty_groups",
    "raw_bytes",
    "bsc_blocks",
    "bsc_wire_bytes",
    "zstd3_wire_bytes",
    "zstd10_wire_bytes",
    "selected_wire_bytes",
    "selected_bsc_groups",
    "selected_zstd3_groups",
    "selected_zstd10_groups",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def named_paths(values: list[str], option: str) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        name, separator, path = value.partition("=")
        require(bool(separator and name and path), f"bad {option} value: {value}")
        require(name not in result, f"duplicate {option} name: {name}")
        result[name] = Path(path)
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def load_summary(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    require(bool(rows), f"{path}: no summary rows")
    result: list[dict[str, Any]] = []
    for source_row in rows:
        require(source_row["exact"] == "true", f"{path}: non-exact row")
        row: dict[str, Any] = dict(source_row)
        for field in INTEGER_FIELDS:
            row[field] = int(source_row[field])
        for field in (
            "bsc_encode_seconds",
            "bsc_decode_seconds",
            "zstd3_encode_seconds",
            "zstd10_encode_seconds",
        ):
            row[field] = float(source_row[field])
        require(
            row["selected_bsc_groups"]
            + row["selected_zstd3_groups"]
            + row["selected_zstd10_groups"]
            == row["nonempty_groups"],
            f"{path}: selected group count differs in {row['group_tus']}",
        )
        result.append(row)
    return result


def validate_detail(path: Path, summaries: list[dict[str, Any]]) -> None:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        require(row["exact"] == "true", f"{path}: non-exact detail row")
        grouped[row["group_tus"]].append(row)
    require(set(grouped) == {row["group_tus"] for row in summaries},
            f"{path}: summary/detail group sets differ")
    for summary in summaries:
        label = summary["group_tus"]
        group_rows = grouped[label]
        require(len(group_rows) == summary["groups"],
                f"{path}: group count differs for {label}")
        raw_total = 0
        selected_total = 0
        bsc_total = 0
        zstd3_total = 0
        zstd10_total = 0
        previous_last = 0
        for row in group_rows:
            first = int(row["first_tu"])
            last = int(row["last_tu"])
            require(first == previous_last + 1 and last >= first,
                    f"{path}: discontinuous TU range for {label}")
            previous_last = last
            raw_total += int(row["raw_bytes"])
            selected_total += int(row["selected_wire_bytes"])
            bsc_total += int(row["bsc_wire_bytes"])
            zstd3_total += int(row["zstd3_wire_bytes"])
            zstd10_total += int(row["zstd10_wire_bytes"])
            require(
                selected_total == int(row["cumulative_selected_wire_bytes"]),
                f"{path}: bad cumulative watermark for {label}",
            )
            candidate_by_codec = {
                "empty": 0,
                "bsc": int(row["bsc_wire_bytes"]),
                "zstd3": int(row["zstd3_wire_bytes"]),
                "zstd10": int(row["zstd10_wire_bytes"]),
            }
            require(
                row["selected_codec"] in candidate_by_codec,
                f"{path}: unknown selected codec for {label}",
            )
            selected = int(row["selected_wire_bytes"])
            require(
                selected == candidate_by_codec[row["selected_codec"]],
                f"{path}: selector size differs from named codec for {label}",
            )
            require(
                selected
                == min(
                    int(row["bsc_wire_bytes"]),
                    int(row["zstd3_wire_bytes"]),
                    int(row["zstd10_wire_bytes"]),
                ),
                f"{path}: selector did not choose the smallest group for {label}",
            )
        require(previous_last == summary["tus"],
                f"{path}: final TU differs for {label}")
        require(raw_total == summary["raw_bytes"],
                f"{path}: raw detail sum differs for {label}")
        require(selected_total == summary["selected_wire_bytes"],
                f"{path}: selected detail sum differs for {label}")
        require(bsc_total == summary["bsc_wire_bytes"],
                f"{path}: BSC detail sum differs for {label}")
        require(zstd3_total == summary["zstd3_wire_bytes"],
                f"{path}: zstd3 detail sum differs for {label}")
        require(zstd10_total == summary["zstd10_wire_bytes"],
                f"{path}: zstd10 detail sum differs for {label}")


def comma(value: int) -> str:
    return f"{value:,}"


def signed(value: int) -> str:
    return f"{value:+,}"


def markdown_table(headers: tuple[str, ...], rows: list[tuple[str, ...]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join("---:" if index else "---" for index in range(len(headers))) + "|",
    ]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", action="append", required=True)
    parser.add_argument("--detail", action="append", required=True)
    parser.add_argument("--raw", action="append", required=True)
    parser.add_argument("--lengths", action="append", required=True)
    parser.add_argument("--component-summary", type=Path, required=True)
    parser.add_argument("--precomputed", type=Path, required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--libbsc-commit", required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()

    summaries = named_paths(args.summary, "--summary")
    details = named_paths(args.detail, "--detail")
    raw_inputs = named_paths(args.raw, "--raw")
    length_inputs = named_paths(args.lengths, "--lengths")
    require(
        set(summaries) == set(details) == set(raw_inputs) == set(length_inputs),
        "summary/detail/raw/length corpus sets differ",
    )
    component = json.loads(args.component_summary.read_text())
    precomputed = json.loads(args.precomputed.read_text())
    component_by_name = {row["name"]: row for row in component["corpora"]}
    precomputed_by_name = {row["name"]: row for row in precomputed["corpora"]}

    output: dict[str, Any] = {
        "schema": "p29-bsc-tu-granularity-v1",
        "wire_format": {
            "group_outer_frame_bytes": 4,
            "group_selector_bytes": 0,
            "group_header": "packed u32: codec kind in high 3 bits, payload length in low 29 bits",
            "bsc_block_header_bytes": 28,
            "bsc_max_raw_block_bytes": 64 * 1024 * 1024,
            "bsc": "libbsc 3.3.12 BWT + QLFC adaptive, default LZP, fast mode",
            "fallbacks": ["zstd-3", "zstd-10"],
        },
        "measurement": {
            "host": args.host,
            "openmp_threads": 1,
            "libbsc_version": "3.3.12",
            "libbsc_commit": args.libbsc_commit,
            "benchmark_source_sha256": sha256(
                Path(__file__).with_name("bsc_block_granularity.cpp")
            ),
        },
        "corpora": [],
    }
    markdown_sections: list[str] = []
    for name in summaries:
        rows = load_summary(summaries[name])
        validate_detail(details[name], rows)
        require(raw_inputs[name].stat().st_size == rows[0]["raw_bytes"],
                f"{name}: retained raw size differs")
        require(name in component_by_name and name in precomputed_by_name,
                f"missing P29 metadata for {name}")
        p29 = component_by_name[name]
        reference = precomputed_by_name[name]
        literal_wire = int(p29["family_wire_bytes"]["literal"])
        complete_wire = int(p29["wire_bytes"])
        fixed_wire = complete_wire - literal_wire
        # The precomputed report replaces all material lanes; this experiment replaces
        # only the literal lane. Its fixed part is therefore complete-minus-literal,
        # not the precomputed report's outside-all-material fixed value.
        whole_z19 = int(reference["whole_ii_z19_bytes"])
        gate = int(reference["cold_gate_bytes"])
        one = next(row for row in rows if row["group_tus"] == "1")
        full = next(row for row in rows if row["group_tus"] == "full")
        best_selected = min(row["selected_wire_bytes"] for row in rows)
        denominator = one["selected_wire_bytes"] - full["selected_wire_bytes"]
        rendered_rows: list[tuple[str, ...]] = []
        enriched: list[dict[str, Any]] = []
        for row in rows:
            selected = row["selected_wire_bytes"]
            projected = fixed_wire + selected
            delta = projected - gate
            captured = None
            if denominator > 0:
                captured = (one["selected_wire_bytes"] - selected) / denominator
            enriched_row = dict(row)
            enriched_row.update(
                {
                    "projected_complete_bytes": projected,
                    "projected_over_whole_ii_z19": projected / whole_z19,
                    "delta_to_cold_gate_bytes": delta,
                    "passes_cold_gate": delta <= 0,
                    "extra_lane_bytes_over_best_measured": selected - best_selected,
                    "fraction_of_one_to_full_gain": captured,
                }
            )
            enriched.append(enriched_row)
            rendered_rows.append(
                (
                    row["group_tus"],
                    comma(selected),
                    comma(projected),
                    f"{projected / whole_z19:.3f}x",
                    signed(delta),
                    comma(selected - best_selected),
                    f"{row['selected_bsc_groups']}/{row['selected_zstd3_groups']}/"
                    f"{row['selected_zstd10_groups']}",
                )
            )
        numeric_passing = [
            row
            for row in enriched
            if row["group_tus"] != "full" and row["passes_cold_gate"]
        ]
        smallest_passing = (
            min(numeric_passing, key=lambda row: int(row["group_tus"]))["group_tus"]
            if numeric_passing
            else None
        )
        corpus_output = {
            "name": name,
            "summary": str(summaries[name]),
            "detail": str(details[name]),
            "raw_input": str(raw_inputs[name]),
            "raw_sha256": sha256(raw_inputs[name]),
            "length_input": str(length_inputs[name]),
            "length_sha256": sha256(length_inputs[name]),
            "tus": rows[0]["tus"],
            "residual_raw_bytes": rows[0]["raw_bytes"],
            "p29_complete_wire_bytes": complete_wire,
            "p29_literal_wire_bytes": literal_wire,
            "fixed_nonliteral_wire_bytes": fixed_wire,
            "whole_ii_z19_bytes": whole_z19,
            "cold_gate_bytes": gate,
            "smallest_fixed_group_passing_cold_gate": smallest_passing,
            "best_measured_group_tus": min(
                enriched, key=lambda row: row["selected_wire_bytes"]
            )["group_tus"],
            "rows": enriched,
        }
        output["corpora"].append(corpus_output)
        markdown_sections.append(
            f"## {name}\n\n"
            f"Exact residual: `{rows[0]['raw_bytes']:,}` raw bytes across "
            f"`{rows[0]['tus']:,}` TUs. Causal P29 literal wire: "
            f"`{literal_wire:,}` bytes. Fixed complete-P29 bytes outside that lane: "
            f"`{fixed_wire:,}`. Whole-program zstd-19-long: `{whole_z19:,}`; "
            f"110% gate: `{gate:,}`.\n\n"
            + markdown_table(
                (
                    "TUs/group",
                    "selected residual",
                    "projected complete",
                    "/ whole z19",
                    "gate delta",
                    "lane bytes over best",
                    "groups BSC/z3/z10",
                ),
                rendered_rows,
            )
        )

    by_name = {row["name"]: row for row in output["corpora"]}
    duckdb = by_name["DuckDB"]
    godot = by_name["Godot"]
    duckdb_rows = {row["group_tus"]: row for row in duckdb["rows"]}
    godot_rows = {row["group_tus"]: row for row in godot["rows"]}
    interpretation = f"""# Exact BSC residual granularity across TU groups

## Result

Most of the build-wide BWT gain does **not** require the complete build. With an exact packed-header
selector over product-shaped BSC, zstd-3, and zstd-10 frames:

- DuckDB falls from `{duckdb_rows['1']['selected_wire_bytes']:,}` residual bytes at one TU/frame to
  `{duckdb_rows['10']['selected_wire_bytes']:,}` at 10 TUs, `{duckdb_rows['100']['selected_wire_bytes']:,}`
  at 100 TUs, and `{duckdb_rows['full']['selected_wire_bytes']:,}` for one complete-build group.
- Godot falls from `{godot_rows['1']['selected_wire_bytes']:,}` to
  `{godot_rows['10']['selected_wire_bytes']:,}`, `{godot_rows['100']['selected_wire_bytes']:,}`, and
  `{godot_rows['full']['selected_wire_bytes']:,}` at the same boundaries.
- A fixed 100-TU DuckDB grouping projects to `{duckdb_rows['100']['projected_complete_bytes']:,}`
  complete bytes, only `{duckdb_rows['100']['delta_to_cold_gate_bytes']:,}` bytes above its gate.
  Within this coarse sweep, the first passing fixed size is `{duckdb['smallest_fixed_group_passing_cold_gate']}` TUs, at
  `{duckdb_rows[str(duckdb['smallest_fixed_group_passing_cold_gate'])]['projected_complete_bytes']:,}`
  complete bytes.
- DuckDB's 500-TU partition is `{duckdb_rows['full']['selected_wire_bytes'] - duckdb_rows['500']['selected_wire_bytes']:,}`
  bytes smaller than one 689-TU BSC block. BWT statistics benefit slightly from a boundary here;
  "one monolithic block" is therefore not automatically the compression optimum.

The engineering implication is a bounded precompute window with every group charged when its first
TU becomes eligible. A focused 104--124 sweep and complete P29 replay subsequently selected one
fixed **112-TU** boundary: it clears DuckDB cold while preserving both Godot prefix gates. See
`P29-BSC-GROUP-INTEGRATION.md` for the actual decoder-verified totals and speed repetitions.

## Exact wire and accounting

Every nonempty group carries one packed four-byte header and the selected payload. The upper three
header bits select the codec and the lower 29 bits carry the payload length.
BSC payloads use one or more self-describing 28-byte libbsc blocks capped at 64 MiB raw. The decoder
reads only the selected payload, independently reconstructs the exact raw residual, and byte-compares
it before the row is accepted. Empty groups carry no residual frame.

The complete projections in this table replace only the exact P29 literal-lane wire and retain every
other P29 byte. Full group-by-group watermarks are retained in the detail TSVs and validated against
each summary row. The later 112-TU row is integrated into P29 and reported separately.

The sweep ran on `{args.host}` with `OMP_NUM_THREADS=1`, libbsc 3.3.12 commit
`{args.libbsc_commit}`. Timing columns describe that host; the size and exact-replay results are the
primary result.

{markdown_sections[0]}

{markdown_sections[1]}

## Scope

This experiment answers the granularity question for BSC only. The previously measured ZPAQ m5
build-wide lane projection remains much smaller, while being a different compression point. The
complete follow-up makes 112-TU residual groups first-class P29 frames, replays them through the
independent decoder, and records the compressed-input watermark at every TU boundary.
"""

    args.json.write_text(json.dumps(output, indent=2) + "\n")
    args.markdown.write_text(interpretation)
    print(f"validated {sum(len(row['rows']) for row in output['corpora'])} summary rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
