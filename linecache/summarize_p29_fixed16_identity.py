#!/usr/bin/env python3
"""Validate and summarize the corrected-P29 fixed-16 identity sweep."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

import summarize_p29_prefix_matrix as common


def parse_cell(root: Path, expected: dict[str, str]) -> dict[str, Any]:
    identity = json.loads((root / "prefix-identity.json").read_text())
    common.require(identity.get("schema") == "p29-prefix-identity-v1", "wrong identity schema")
    common.verify_artifact_hashes(identity)
    meta = common.read_key_values(root / "run.meta")
    full_curve = common.read_tsv(root / "full/curve.tsv")
    prefix_curve = common.read_tsv(root / "prefix/curve.tsv")
    full_components = common.read_tsv(root / "full/components.tsv")
    prefix_components = common.read_tsv(root / "prefix/components.tsv")

    full_tus = int(expected["tu_count"])
    full_raw = int(expected["raw_bytes"])
    prefix_tus = min(112, full_tus)
    mode = "suffix-blind" if prefix_tus < full_tus else "complete-program"
    common.require(len(full_curve) == full_tus, "complete curve TU count differs")
    common.require(len(prefix_curve) == prefix_tus, "prefix curve TU count differs")
    common.require(identity["prefix_tus"] == prefix_tus, "identity prefix extent differs")
    common.require(identity["prefix_mode"] == mode and meta["prefix_mode"] == mode, "mode differs")
    common.require(identity["curve_identical"] and identity["components_identical"], "prefix differs")
    common.require(full_curve[:prefix_tus] == prefix_curve, "complete/prefix curves differ")
    common.require(full_components[:prefix_tus] == prefix_components, "components differ")
    common.require(all(row["exact"] == "true" for row in full_curve + prefix_curve), "non-exact row")
    common.require(int(full_curve[-1]["cumulative_raw_bytes"]) == full_raw, "raw total differs")
    common.verify_component_rows(full_components, full_tus)
    common.verify_component_rows(prefix_components, prefix_tus)

    full_wire = int(full_curve[-1]["cumulative_wire_bytes"])
    prefix_wire = int(prefix_curve[-1]["cumulative_wire_bytes"])
    common.require(prefix_wire == identity["prefix_wire_bytes"], "prefix wire differs")
    stdout = (root / "full/grouped.stdout").read_text()
    stderr = (root / "full/grouped.stderr").read_text()
    total = int(common.one(common.TOTAL_RE, stdout, "P29 total").group(1))
    common.require(total == full_wire, "reported complete total differs")
    common.require("byte-exact=OK" in stdout, "complete replay did not report exact")
    endpoint = common.one(common.ENDPOINT_RE, stderr, "P29 endpoint proxy")
    loaded = common.one(common.LOADED_RE, stderr, "P29 loaded census")
    common.require(int(loaded.group(2)) == full_tus, "loaded TU census differs")
    common.require(int(loaded.group(3)) == full_raw, "loaded raw census differs")

    hashes = common.read_hashes(root / "tooling.sha256")
    binary_hashes = [
        digest
        for name, digest in hashes.items()
        if Path(name).name.startswith("codec50") and Path(name).suffix != ".cpp"
    ]
    source_hashes = [digest for name, digest in hashes.items() if Path(name).name == "codec50.cpp"]
    common.require(len(binary_hashes) == 1 and len(source_hashes) == 1, "P29 provenance differs")

    half = max(1, prefix_tus // 2)
    quarter = max(1, 3 * prefix_tus // 4)

    def extent(rows: list[dict[str, str]], lo: int, hi: int, field: str) -> int:
        return sum(int(row[field]) for row in rows[lo:hi])

    first_raw = extent(prefix_curve, 0, half, "raw_bytes")
    first_wire = extent(prefix_curve, 0, half, "wire_bytes")
    last_raw = extent(prefix_curve, quarter, prefix_tus, "raw_bytes")
    last_wire = extent(prefix_curve, quarter, prefix_tus, "wire_bytes")
    trajectory = (last_wire / last_raw) / (first_wire / first_raw)

    component_fields = [
        name
        for name in prefix_components[0]
        if name.endswith("_raw_bytes") or name.endswith("_wire_bytes")
        if name not in {"cumulative_raw_bytes", "cumulative_wire_bytes"}
    ]
    row: dict[str, Any] = {
        "id": expected["id"],
        "name": expected["name"],
        "tus": full_tus,
        "raw_bytes": full_raw,
        "manifest_sha256": expected["manifest_sha256"],
        "probe_tus": prefix_tus,
        "probe_mode": mode,
        "probe_raw_bytes": int(prefix_curve[-1]["cumulative_raw_bytes"]),
        "probe_wire_bytes": prefix_wire,
        "complete_wire_bytes": full_wire,
        "probe_p29_trajectory": trajectory,
        "probe_first_half_wire_bpr": first_wire / first_raw,
        "probe_last_quarter_wire_bpr": last_wire / last_raw,
        "p29_c_proxy_gbps": float(endpoint.group(1)),
        "p29_f_proxy_gbps": float(endpoint.group(2)),
        "loaded_interned_s": float(loaded.group(1)),
        "p29_source_sha256": source_hashes[0],
        "p29_binary_sha256": binary_hashes[0],
        "p29_commit": meta["p29_commit"],
        "libbsc_commit": meta["libbsc_commit"],
        "prefix_identity": True,
        "exact": True,
    }
    for field in component_fields:
        row[f"probe_{field}"] = common.sum_column(prefix_components, field)
    return row


def markdown(rows: list[dict[str, Any]], summary: dict[str, Any]) -> str:
    lines = [
        "# Corrected P29 fixed-16 prefix identity",
        "",
        f"- Exact cells: **{summary['exact_cells']}/{summary['cells']}**.",
        f"- Prefix-identical cells: **{summary['prefix_identity_cells']}/{summary['cells']}**.",
        f"- Corrected complete P29: **{summary['complete_wire_bytes']:,} B**.",
        f"- Corrected TU112/complete-program probes: **{summary['probe_wire_bytes']:,} B**.",
        "",
        "| corpus | TUs | probe raw MB | probe P29 MB | complete P29 MB | trajectory | mode |",
        "|---|---:|---:|---:|---:|---:|:---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['name']} | {row['tus']} | {row['probe_raw_bytes']/1e6:.3f} | "
            f"{row['probe_wire_bytes']/1e6:.3f} | {row['complete_wire_bytes']/1e6:.3f} | "
            f"{row['probe_p29_trajectory']:.4f} | {row['probe_mode']} |"
        )
    lines.extend(
        [
            "",
            "These are corrected deterministic size/feature rows. Process timings remain diagnostic",
            "and are not the isolated common-input selector measurement.",
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
    ledger = common.read_tsv(root / "fixed16-ledger.tsv")
    common.require(len(ledger) == args.expected_cells, "fixed-16 ledger count differs")
    expected = {row["id"]: row for row in ledger}
    paths = sorted((root / "cells").glob("*/prefix-identity.json"))
    common.require(len(paths) == args.expected_cells, "fixed-16 result count differs")
    rows = [parse_cell(path.parent, expected[path.parent.name]) for path in paths]
    rows.sort(key=lambda row: list(expected).index(row["id"]))
    source_hashes = {row["p29_source_sha256"] for row in rows}
    binary_hashes = {row["p29_binary_sha256"] for row in rows}
    common.require(len(source_hashes) == 1 and len(binary_hashes) == 1, "P29 tooling drifts")
    summary = {
        "schema": "p29-fixed16-prefix-identity-v1",
        "cells": len(rows),
        "raw_bytes": sum(row["raw_bytes"] for row in rows),
        "probe_wire_bytes": sum(row["probe_wire_bytes"] for row in rows),
        "complete_wire_bytes": sum(row["complete_wire_bytes"] for row in rows),
        "exact_cells": sum(row["exact"] for row in rows),
        "prefix_identity_cells": sum(row["prefix_identity"] for row in rows),
        "p29_source_sha256": next(iter(source_hashes)),
        "p29_binary_sha256": next(iter(binary_hashes)),
        "ledger_sha256": common.sha256_file(root / "fixed16-ledger.tsv"),
        "timing_scope": "diagnostic two-process research pass",
    }
    with (root / "p29-fixed16-measurements.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, delimiter="\t", lineterminator="\n", fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (root / "p29-fixed16-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    (root / "P29-FIXED16-PREFIX-IDENTITY.md").write_text(markdown(rows, summary))
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
