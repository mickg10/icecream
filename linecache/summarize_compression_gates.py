#!/usr/bin/env python3
"""Join whole-stream zstd references to the exact P29 per-TU ledger."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Sequence

from run_complete_tu_matrix import CORPORA


FULL_ALLOWANCE_NUMERATOR = 11
FULL_ALLOWANCE_DENOMINATOR = 10
CHECKPOINTS = (100, 200)


def allowance_110pct(value: int) -> int:
    return value * FULL_ALLOWANCE_NUMERATOR // FULL_ALLOWANCE_DENOMINATOR


def load_p29(path: Path) -> dict[str, list[dict]]:
    grouped: dict[str, list[dict]] = {}
    with path.open(newline="") as source:
        for raw in csv.DictReader(source, delimiter="\t"):
            if raw["exact"] != "true":
                raise ValueError(f"{path}: inexact P29 row")
            row = {
                "tu": int(raw["tu"]),
                "raw_bytes": int(raw["raw_bytes"]),
                "wire_bytes": int(raw["wire_bytes"]),
                "cumulative_raw_bytes": int(raw["cumulative_raw_bytes"]),
                "cumulative_wire_bytes": int(raw["cumulative_wire_bytes"]),
            }
            grouped.setdefault(raw["corpus"], []).append(row)
    for corpus, rows in grouped.items():
        cumulative_raw = cumulative_wire = 0
        for ordinal, row in enumerate(rows, 1):
            cumulative_raw += row["raw_bytes"]
            cumulative_wire += row["wire_bytes"]
            if (
                row["tu"] != ordinal
                or row["cumulative_raw_bytes"] != cumulative_raw
                or row["cumulative_wire_bytes"] != cumulative_wire
            ):
                raise ValueError(f"{path}: invalid {corpus} row {ordinal}")
    return grouped


def load_references(directory: Path) -> dict[str, dict]:
    reports: dict[str, dict] = {}
    for path in sorted(directory.glob("*-z6long-prefix.json")):
        report = json.loads(path.read_text())
        if (
            report["schema"] != "whole-stream-reference-v1"
            or report["kind"] != "ii"
            or report["level"] != 6
            or report["long_log"] != 31
            or report["threads"] != 1
            or "v1.5.7" not in report["zstd_version"]
        ):
            raise ValueError(f"{path}: incompatible zstd reference")
        if report["name"] in reports:
            raise ValueError(f"{path}: duplicate corpus reference")
        report["artifact_path"] = str(path.resolve())
        reports[report["name"]] = report
    return reports


def reference_at(report: dict, count: int) -> dict | None:
    return next(
        (row for row in report["measurements"] if row["files"] == count),
        None,
    )


def checkpoint(
    p29_rows: Sequence[dict],
    reference: dict,
    requested: int,
) -> dict:
    eligible = len(p29_rows) >= requested
    count = requested if eligible else len(p29_rows)
    measured = reference_at(reference, count)
    if measured is None:
        raise ValueError(f"missing z6-long reference at {count} TUs")
    candidate = p29_rows[count - 1]
    if candidate["cumulative_raw_bytes"] != measured["raw_bytes"]:
        raise ValueError(f"raw prefix differs at {count} TUs")
    z6_bytes = measured["compressed_bytes"]
    wire_bytes = candidate["cumulative_wire_bytes"]
    return {
        "requested_tu": requested,
        "actual_tu": count,
        "eligible": eligible,
        "raw_bytes": measured["raw_bytes"],
        "content_sha256": measured["content_sha256"],
        "z6long_bytes": z6_bytes,
        "p29_wire_bytes": wire_bytes,
        "delta_bytes": wire_bytes - z6_bytes,
        "p29_over_z6long": wire_bytes / z6_bytes,
        "pass": wire_bytes <= z6_bytes if eligible else None,
    }


def aggregate_checkpoints(rows: Sequence[dict], requested: int) -> dict:
    selected = [row[f"tu{requested}"] for row in rows if row[f"tu{requested}"]["eligible"]]
    z6_bytes = sum(row["z6long_bytes"] for row in selected)
    wire_bytes = sum(row["p29_wire_bytes"] for row in selected)
    return {
        "eligible_programs": len(selected),
        "passing_programs": sum(bool(row["pass"]) for row in selected),
        "z6long_bytes": z6_bytes,
        "p29_wire_bytes": wire_bytes,
        "delta_bytes": wire_bytes - z6_bytes,
        "p29_over_z6long": wire_bytes / z6_bytes,
        "aggregate_pass": wire_bytes <= z6_bytes,
    }


def fmt_bytes(value: int | None) -> str:
    return "—" if value is None else f"{value:,}"


def write_tsv(rows: Sequence[dict], path: Path) -> None:
    columns = (
        "corpus",
        "corpus_id",
        "project",
        "tus",
        "raw_ii_bytes",
        "z19long_ii_bytes",
        "cold_110pct_gate_bytes",
        "p29_cold_bytes",
        "p29_over_z19long",
        "cold_pass",
        "tu100_eligible",
        "z6long_tu100_bytes",
        "p29_tu100_bytes",
        "p29_over_z6long_tu100",
        "tu100_pass",
        "tu200_eligible",
        "z6long_tu200_bytes",
        "p29_tu200_bytes",
        "p29_over_z6long_tu200",
        "tu200_pass",
        "raw_source_bytes",
        "z19long_source_bytes",
        "source_110pct_gate_bytes",
        "source_basis",
        "candidate_source_bytes",
        "source_pass",
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=columns, delimiter="\t")
        writer.writeheader()
        for row in rows:
            flattened = {
                key: row.get(key)
                for key in columns
            }
            for count in CHECKPOINTS:
                point = row[f"tu{count}"]
                flattened.update(
                    {
                        f"tu{count}_eligible": str(point["eligible"]).lower(),
                        f"z6long_tu{count}_bytes": point["z6long_bytes"],
                        f"p29_tu{count}_bytes": point["p29_wire_bytes"],
                        f"p29_over_z6long_tu{count}": point["p29_over_z6long"],
                        f"tu{count}_pass": (
                            "" if point["pass"] is None else str(point["pass"]).lower()
                        ),
                    }
                )
            flattened["cold_pass"] = str(row["cold_pass"]).lower()
            flattened["candidate_source_bytes"] = ""
            flattened["source_pass"] = ""
            writer.writerow(flattened)


def write_markdown(report: dict, path: Path) -> None:
    aggregate = report["aggregate"]
    lines = [
        "# Compression tournament gate matrix: current P29 control",
        "",
        "This is the first matrix against the owner's binding whole-stream gates. "
        "Every zstd reference is one frame over ordered concatenated content, not a "
        "sum of per-TU frames. Installed-package bytes are not present in P29.",
        "",
        "## Aggregate status",
        "",
        "| gate | eligible | passing | reference | P29 | P29/reference | result |",
        "|---|---:|---:|---:|---:|---:|:---:|",
        (
            f"| cold `.ii` ≤110% z19-long per program | 16 | "
            f"{aggregate['cold']['passing_programs']} | "
            f"{aggregate['cold']['z19long_bytes']:,} | "
            f"{aggregate['cold']['p29_wire_bytes']:,} | "
            f"{aggregate['cold']['p29_over_z19long']:.3f} | "
            f"{'PASS aggregate / FAIL per-program' if aggregate['cold']['aggregate_pass'] else 'FAIL'} |"
        ),
    ]
    for count in CHECKPOINTS:
        point = aggregate[f"tu{count}"]
        lines.append(
            f"| TU {count} ≤ z6-long per program | "
            f"{point['eligible_programs']} | {point['passing_programs']} | "
            f"{point['z6long_bytes']:,} | {point['p29_wire_bytes']:,} | "
            f"{point['p29_over_z6long']:.3f} | "
            f"{'PASS aggregate / FAIL per-program' if point['aggregate_pass'] else 'FAIL aggregate'} |"
        )
    lines.extend(
        [
            "| cold raw source ≤110% z19-long | 16 | 0 measured | "
            f"{aggregate['source']['z19long_bytes']:,} | — | — | OPEN |",
            "",
            "Aggregate passing does not satisfy the goal: each eligible program must pass.",
            "",
            "## Per-program matrix",
            "",
            "| program | cold P29/z19 | cold | TU100 P29/z6 | TU100 | TU200 P29/z6 | TU200 | raw source |",
            "|---|---:|:---:|---:|:---:|---:|:---:|:---:|",
        ]
    )
    for row in report["rows"]:
        checkpoints = []
        for count in CHECKPOINTS:
            point = row[f"tu{count}"]
            if point["eligible"]:
                checkpoints.extend(
                    [
                        f"{point['p29_over_z6long']:.3f}",
                        "yes" if point["pass"] else "no",
                    ]
                )
            else:
                checkpoints.extend(
                    [
                        f"final@{point['actual_tu']}: {point['p29_over_z6long']:.3f}",
                        "n/a",
                    ]
                )
        lines.append(
            f"| {row['project']} | {row['p29_over_z19long']:.3f} | "
            f"{'yes' if row['cold_pass'] else 'no'} | "
            f"{checkpoints[0]} | {checkpoints[1]} | "
            f"{checkpoints[2]} | {checkpoints[3]} | open |"
        )
    lines.extend(
        [
            "",
            "## Exact byte ledger",
            "",
            "| program | z19 `.ii` | 110% gate | P29 cold | z6 TU100 | P29 TU100 | z6 TU200 | P29 TU200 | z19 source | source gate |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in report["rows"]:
        checkpoints = []
        for count in CHECKPOINTS:
            point = row[f"tu{count}"]
            checkpoints.extend(
                [
                    fmt_bytes(point["z6long_bytes"] if point["eligible"] else None),
                    fmt_bytes(point["p29_wire_bytes"] if point["eligible"] else None),
                ]
            )
        lines.append(
            f"| {row['project']} | {row['z19long_ii_bytes']:,} | "
            f"{row['cold_110pct_gate_bytes']:,} | {row['p29_cold_bytes']:,} | "
            f"{checkpoints[0]} | {checkpoints[1]} | "
            f"{checkpoints[2]} | {checkpoints[3]} | "
            f"{row['z19long_source_bytes']:,} | {row['source_110pct_gate_bytes']:,} |"
        )
    lines.extend(
        [
            "",
            "## Provenance",
            "",
            f"- zstd: `{report['reference_contract']['zstd_version']}`",
            "- zstd prefix command: `zstd -6 --long=31 -c` with one thread",
            "- cold reference: retained corpus catalog `zstd -19 --long=31` values",
            "- candidate: exact first-repetition RBASE-P29 per-TU export",
            "- all measured prefix raw-byte totals match the candidate ledger exactly",
            "",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--catalog", type=Path, required=True)
    value.add_argument("--p29-tsv", type=Path, required=True)
    value.add_argument("--z6-reference-dir", type=Path, required=True)
    value.add_argument("--output-json", type=Path, required=True)
    value.add_argument("--output-tsv", type=Path, required=True)
    value.add_argument("--output-md", type=Path, required=True)
    return value


def main() -> int:
    args = parser().parse_args()
    catalog_raw = json.loads(args.catalog.read_text())
    catalog = {row["corpus_id"]: row for row in catalog_raw["corpora"]}
    p29 = load_p29(args.p29_tsv)
    references = load_references(args.z6_reference_dir)
    expected_ids = {corpus_id for _, corpus_id in CORPORA}
    if set(references) != expected_ids:
        raise ValueError(
            f"reference coverage differs: {sorted(set(references) ^ expected_ids)}"
        )

    rows = []
    for corpus, corpus_id in CORPORA:
        p29_rows = p29[corpus]
        metadata = catalog[corpus_id]
        reference = references[corpus_id]
        if len(p29_rows) != metadata["TU"] or reference["total_files"] != len(p29_rows):
            raise ValueError(f"{corpus}: TU count differs")
        final = p29_rows[-1]
        if final["cumulative_raw_bytes"] != metadata["bytes_ii"]:
            raise ValueError(f"{corpus}: full raw bytes differ")
        z19_ii = metadata["z19long_ii"]
        source_basis = metadata.get("source_subset_llvm_only", metadata)
        z19_source = source_basis["z19long_src"]
        if z19_ii is None or z19_source is None:
            raise ValueError(f"{corpus}: missing whole-program reference")
        gate = allowance_110pct(z19_ii)
        row = {
            "corpus": corpus,
            "corpus_id": corpus_id,
            "project": metadata["project"],
            "tus": len(p29_rows),
            "raw_ii_bytes": metadata["bytes_ii"],
            "z19long_ii_bytes": z19_ii,
            "cold_110pct_gate_bytes": gate,
            "p29_cold_bytes": final["cumulative_wire_bytes"],
            "p29_over_z19long": final["cumulative_wire_bytes"] / z19_ii,
            "cold_pass": final["cumulative_wire_bytes"] <= gate,
            "raw_source_bytes": source_basis["bytes_src"],
            "z19long_source_bytes": z19_source,
            "source_110pct_gate_bytes": allowance_110pct(z19_source),
            "source_basis": (
                "built llvm/ subtree"
                if source_basis is not metadata
                else "filtered project source set"
            ),
            "candidate_source_bytes": None,
            "source_pass": None,
            "tu100": checkpoint(p29_rows, reference, 100),
            "tu200": checkpoint(p29_rows, reference, 200),
        }
        rows.append(row)

    z19_total = sum(row["z19long_ii_bytes"] for row in rows)
    p29_total = sum(row["p29_cold_bytes"] for row in rows)
    source_total = sum(row["z19long_source_bytes"] for row in rows)
    aggregate = {
        "cold": {
            "programs": len(rows),
            "passing_programs": sum(row["cold_pass"] for row in rows),
            "z19long_bytes": z19_total,
            "allowance_110pct_bytes": allowance_110pct(z19_total),
            "p29_wire_bytes": p29_total,
            "p29_over_z19long": p29_total / z19_total,
            "aggregate_pass": p29_total <= allowance_110pct(z19_total),
        },
        "tu100": aggregate_checkpoints(rows, 100),
        "tu200": aggregate_checkpoints(rows, 200),
        "source": {
            "programs": len(rows),
            "measured_candidate_programs": 0,
            "z19long_bytes": source_total,
            "allowance_110pct_bytes": allowance_110pct(source_total),
            "candidate_wire_bytes": None,
            "pass": None,
        },
    }
    versions = {report["zstd_version"] for report in references.values()}
    if len(versions) != 1:
        raise ValueError("zstd reference versions differ")
    report = {
        "schema": "compression-tournament-gates-v1",
        "goal": {
            "cold": "candidate <= 1.10 * whole-program zstd-19 --long=31",
            "prefix": "candidate <= whole-prefix zstd-6 --long=31 at TU100/TU200",
            "scope": "each eligible program must pass; aggregates are secondary",
            "installed_assets": "reported separately and not charged per build",
        },
        "reference_contract": {
            "zstd_version": next(iter(versions)),
            "prefix_level": 6,
            "cold_level": 19,
            "long_log": 31,
            "threads": 1,
            "content": "ordered byte concatenation; no tar or path metadata",
            "all_prefix_raw_totals_match_p29": True,
        },
        "candidate": "RBASE-P29 exact incremental empty start",
        "aggregate": aggregate,
        "rows": rows,
        "reference_reports": list(references.values()),
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    write_tsv(rows, args.output_tsv)
    write_markdown(report, args.output_md)
    print(
        json.dumps(
            {
                "cold_pass": f"{aggregate['cold']['passing_programs']}/16",
                "tu100_pass": (
                    f"{aggregate['tu100']['passing_programs']}/"
                    f"{aggregate['tu100']['eligible_programs']}"
                ),
                "tu200_pass": (
                    f"{aggregate['tu200']['passing_programs']}/"
                    f"{aggregate['tu200']['eligible_programs']}"
                ),
                "source": "open",
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
