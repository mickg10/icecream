#!/usr/bin/env python3
"""Extract causal TU112 selector features from the audited policy-B census.

This is a thin enrichment of :mod:`analyze_policy_b_size_census`: that module
first revalidates complete sizes, exact replay, prefix identity, and tooling.
Only then do we aggregate P29 component/trajectory rows and current-GRZ group
counters available at the frozen probe boundary.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any, Iterable

import analyze_policy_b_size_census as audited


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def sum_field(rows: Iterable[dict[str, str]], field: str) -> int:
    return sum(int(row[field]) for row in rows)


def ratio(numerator: int | float, denominator: int | float, description: str) -> float:
    require(denominator > 0, f"zero denominator for {description}")
    result = numerator / denominator
    require(math.isfinite(result), f"non-finite {description}")
    return result


def p29_features(cell: Path, probe_tus: int, probe_raw: int, probe_wire: int) -> dict[str, Any]:
    curve = read_tsv(cell / "id/prefix/curve.tsv")
    components = read_tsv(cell / "id/prefix/components.tsv")
    require(len(curve) == probe_tus == len(components), f"P29 probe extent differs: {cell}")
    require(int(curve[-1]["cumulative_raw_bytes"]) == probe_raw, f"P29 raw differs: {cell}")
    require(int(curve[-1]["cumulative_wire_bytes"]) == probe_wire, f"P29 wire differs: {cell}")

    half = max(1, probe_tus // 2)
    quarter = max(1, 3 * probe_tus // 4)
    first_raw = sum_field(curve[:half], "raw_bytes")
    first_wire = sum_field(curve[:half], "wire_bytes")
    last_raw = sum_field(curve[quarter:], "raw_bytes")
    last_wire = sum_field(curve[quarter:], "wire_bytes")
    result: dict[str, Any] = {
        "p29_probe_wire_bpr": ratio(probe_wire, probe_raw, "P29 wire/raw"),
        "p29_probe_first_half_wire_bpr": ratio(first_wire, first_raw, "P29 first-half wire/raw"),
        "p29_probe_last_quarter_wire_bpr": ratio(last_wire, last_raw, "P29 last-quarter wire/raw"),
        "p29_probe_trajectory": ratio(
            ratio(last_wire, last_raw, "P29 last-quarter wire/raw"),
            ratio(first_wire, first_raw, "P29 first-half wire/raw"),
            "P29 trajectory",
        ),
    }
    fields = [
        field
        for field in components[0]
        if field.endswith("_raw_bytes") or field.endswith("_wire_bytes")
        if field not in {"raw_bytes", "wire_bytes", "cumulative_raw_bytes", "cumulative_wire_bytes"}
    ]
    for field in fields:
        value = sum_field(components, field)
        result[f"p29_probe_{field}"] = value
        extent = probe_raw if field.endswith("_raw_bytes") else probe_wire
        result[f"p29_probe_{field.removesuffix('_bytes')}_fraction"] = ratio(
            value, extent, f"P29 {field} fraction"
        )
    return result


def grz_features(cell: Path, probe_tus: int, probe_raw: int, probe_wire: int) -> dict[str, Any]:
    curve = read_tsv(cell / "grz/curve.tsv")
    require(bool(curve), f"empty GRZ curve: {cell}")
    require(sum_field(curve, "out_bytes") == probe_raw, f"GRZ raw differs: {cell}")
    require(int(curve[-1]["tu_hi"]) == probe_tus, f"GRZ TU extent differs: {cell}")
    framing = probe_wire - sum_field(curve, "comp_bytes")
    require(framing > 0, f"GRZ framing differs: {cell}")
    samples = sum_field(curve, "anchor_samples")
    occupied = sum_field(curve, "anchor_occupied")
    usable = sum_field(curve, "anchor_usable")
    collisions = sum_field(curve, "anchor_collisions")
    matches = sum_field(curve, "anchor_matches")
    require(0 <= matches <= usable <= occupied <= samples, f"GRZ anchor counters differ: {cell}")
    require(matches + collisions == usable, f"GRZ usable accounting differs: {cell}")
    return {
        "grz_probe_wire_bpr": ratio(probe_wire, probe_raw, "GRZ wire/raw"),
        "grz_probe_group_count": len(curve),
        "grz_probe_framing_bytes": framing,
        "grz_probe_add_bytes": sum_field(curve, "add_bytes"),
        "grz_probe_add_bpr": ratio(sum_field(curve, "add_bytes"), probe_raw, "GRZ ADD/raw"),
        "grz_probe_anchor_samples": samples,
        "grz_probe_anchor_matches": matches,
        "grz_probe_anchor_sample_per_raw": ratio(samples, probe_raw, "GRZ samples/raw"),
        "grz_probe_anchor_occupied_fraction": ratio(occupied, samples, "GRZ occupied/samples"),
        "grz_probe_anchor_usable_fraction": ratio(usable, samples, "GRZ usable/samples"),
        "grz_probe_anchor_match_fraction": ratio(matches, usable, "GRZ matches/usable"),
        "grz_probe_anchor_collision_fraction": ratio(collisions, usable, "GRZ collisions/usable"),
        "grz_probe_close_reasons": ",".join(sorted({row["closed_by"] for row in curve})),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--census-root", required=True, type=Path)
    parser.add_argument("--replay-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expected-cells", default=44, type=int)
    args = parser.parse_args()

    census_root = args.census_root.resolve()
    rows = audited.collect_rows(census_root, args.replay_root.resolve(), args.expected_cells)
    by_identity = {(row["project"], row["profile"]): row for row in rows}
    require(len(by_identity) == args.expected_cells, "audited census identities differ")

    enriched: list[dict[str, Any]] = []
    for row_path in sorted(census_root.glob("*/row.tsv")):
        values = audited.read_key_values(row_path)
        identity = (values["project"], values["profile"])
        base = dict(by_identity[identity])
        cell = row_path.parent
        base.update(
            p29_features(
                cell,
                int(base["probe_tus"]),
                int(base["probe_raw_bytes"]),
                int(base["p29_probe_bytes"]),
            )
        )
        base.update(
            grz_features(
                cell,
                int(base["probe_tus"]),
                int(base["probe_raw_bytes"]),
                int(base["grz_probe_bytes"]),
            )
        )
        base["probe_wire_delta_bpr"] = base["p29_probe_wire_bpr"] - base["grz_probe_wire_bpr"]
        enriched.append(base)
    enriched.sort(key=lambda row: (row["project"], row["profile"]))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, delimiter="\t", lineterminator="\n", fieldnames=list(enriched[0])
        )
        writer.writeheader()
        writer.writerows(enriched)
    summary = {
        "schema": "policy-b-selector-features-v1",
        "cells": len(enriched),
        "projects": len({row["project"] for row in enriched}),
        "profiles": sorted({row["profile"] for row in enriched}),
        "exact_cells": sum(bool(row["exact"]) for row in enriched),
        "prefix_identity_cells": sum(bool(row["prefix_identity"]) for row in enriched),
        "output_sha256": audited.sha256_file(args.output),
    }
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
