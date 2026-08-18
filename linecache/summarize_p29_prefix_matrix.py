#!/usr/bin/env python3
"""Validate and summarize a corrected-P29 chronological-prefix matrix."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
from typing import Any


ENDPOINT_RE = re.compile(
    r"split \(2-proc per-stream proxy\): C-encode ([0-9.]+) GB/s \| "
    r"F-decode ([0-9.]+) GB/s"
)
LOADED_RE = re.compile(r"loaded\+interned ([0-9.]+)s TUs=(\d+) raw=(\d+)")
TOTAL_RE = re.compile(r"\bTOTAL=(\d+)")
GROUP_RE = re.compile(
    r"literal groups: tus_per_group=(\d+) groups=(\d+) workers=(\d+) raw=(\d+) "
    r"wire=(\d+).+selected=\[zstd3=(\d+) bsc=(\d+) zstd10=(\d+)\]"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def read_key_values(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
    return result


def read_hashes(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        result[name.strip()] = digest
    return result


def one(pattern: re.Pattern[str], text: str, description: str) -> re.Match[str]:
    matches = list(pattern.finditer(text))
    require(len(matches) == 1, f"expected one {description}, found {len(matches)}")
    return matches[0]


def elapsed_seconds(value: str) -> float:
    parts = value.split(":")
    require(1 <= len(parts) <= 3, f"bad elapsed time: {value!r}")
    seconds = float(parts[-1])
    if len(parts) >= 2:
        seconds += 60 * int(parts[-2])
    if len(parts) == 3:
        seconds += 3600 * int(parts[0])
    return seconds


def read_time(path: Path) -> dict[str, float | int]:
    values: dict[str, str] = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        elapsed_prefix = "Elapsed (wall clock) time (h:mm:ss or m:ss):"
        if line.startswith(elapsed_prefix):
            values["Elapsed (wall clock) time (h:mm:ss or m:ss)"] = line[
                len(elapsed_prefix) :
            ].strip()
        elif ":" in line:
            key, value = line.rsplit(":", 1)
            values[key] = value.strip()
    elapsed_key = "Elapsed (wall clock) time (h:mm:ss or m:ss)"
    require(elapsed_key in values, f"missing elapsed time: {path}")
    return {
        "wall_s": elapsed_seconds(values[elapsed_key]),
        "user_s": float(values["User time (seconds)"]),
        "system_s": float(values["System time (seconds)"]),
        "max_rss_kib": int(values["Maximum resident set size (kbytes)"]),
    }


def sum_column(rows: list[dict[str, str]], name: str) -> int:
    return sum(int(row[name]) for row in rows)


def verify_component_rows(rows: list[dict[str, str]], expected_rows: int) -> None:
    require(len(rows) == expected_rows, "component row count differs")
    for ordinal, row in enumerate(rows, start=1):
        require(int(row["tu"]) == ordinal and row["exact"] == "true", "bad component row")
        component_sum = sum(
            int(value)
            for name, value in row.items()
            if name.endswith("_wire_bytes")
            and name not in {"wire_bytes", "cumulative_wire_bytes"}
        )
        require(component_sum == int(row["wire_bytes"]), f"component sum differs at TU {ordinal}")


def verify_artifact_hashes(identity: dict[str, Any]) -> None:
    for name, expected in identity["artifacts"].items():
        path = Path(name)
        require(path.is_file(), f"missing identity artifact: {path}")
        require(sha256_file(path) == expected, f"identity artifact hash differs: {path}")


def parse_cell(root: Path, expected: dict[str, str]) -> dict[str, Any]:
    identity = json.loads((root / "prefix-identity.json").read_text())
    require(identity.get("schema") == "p29-prefix-identity-v1", "wrong identity schema")
    verify_artifact_hashes(identity)

    meta = read_key_values(root / "run.meta")
    prep = json.loads((root / "input/preparation.json").read_text())
    full_curve = read_tsv(root / "full/curve.tsv")
    prefix_curve = read_tsv(root / "prefix/curve.tsv")
    full_components = read_tsv(root / "full/components.tsv")
    prefix_components = read_tsv(root / "prefix/components.tsv")

    full_tus = int(expected["tu_count"])
    full_raw = int(expected["raw_bytes"])
    prefix_tus = min(112, full_tus)
    expected_mode = "suffix-blind" if full_tus > 112 else "complete-program"
    require(prep["project"] == expected["project"], "prepared project differs")
    require(prep["profile"] == expected["profile"], "prepared profile differs")
    require(int(prep["tu_count"]) == full_tus, "prepared TU count differs")
    require(int(prep["raw_bytes"]) == full_raw, "prepared raw extent differs")
    require(int(prep["archive_bytes"]) == int(expected["payload_bytes"]), "payload size differs")
    require(prep["archive_sha256"] == expected["payload_sha256"], "payload digest differs")
    require(
        prep["source_manifest_sha256"] == expected["manifest_sha256"],
        "source manifest digest differs",
    )
    require(identity["full_tus"] == full_tus, "identity complete TU count differs")
    require(identity["prefix_tus"] == prefix_tus, "identity probe TU count differs")
    require(identity["group_tus"] == 112, "identity group policy differs")
    require(identity["prefix_mode"] == expected_mode, "identity prefix mode differs")
    require(meta["prefix_mode"] == expected_mode, "run metadata prefix mode differs")
    require(identity["curve_identical"] and identity["components_identical"], "prefix differs")

    require(len(full_curve) == full_tus, "complete curve row count differs")
    require(len(prefix_curve) == prefix_tus, "prefix curve row count differs")
    require(all(row["exact"] == "true" for row in full_curve + prefix_curve), "non-exact curve")
    require(int(full_curve[-1]["cumulative_raw_bytes"]) == full_raw, "complete raw total differs")
    require(
        int(prefix_curve[-1]["cumulative_raw_bytes"]) == identity["prefix_raw_bytes"],
        "prefix raw total differs",
    )
    full_wire = int(full_curve[-1]["cumulative_wire_bytes"])
    prefix_wire = int(prefix_curve[-1]["cumulative_wire_bytes"])
    require(prefix_wire == identity["prefix_wire_bytes"], "prefix wire total differs")
    require(full_curve[:prefix_tus] == prefix_curve, "complete/prefix curves differ")

    verify_component_rows(full_components, full_tus)
    verify_component_rows(prefix_components, prefix_tus)
    require(full_components[:prefix_tus] == prefix_components, "component prefixes differ")
    require(int(full_components[-1]["cumulative_wire_bytes"]) == full_wire, "full ledger differs")

    full_stdout = (root / "full/grouped.stdout").read_text()
    full_stderr = (root / "full/grouped.stderr").read_text()
    total = int(one(TOTAL_RE, full_stdout, "P29 total").group(1))
    require(total == full_wire, "reported P29 total differs")
    endpoint = one(ENDPOINT_RE, full_stderr, "P29 endpoint proxy")
    loaded = one(LOADED_RE, full_stderr, "P29 loaded census")
    require(int(loaded.group(2)) == full_tus and int(loaded.group(3)) == full_raw, "load census differs")
    group = one(GROUP_RE, full_stdout, "P29 literal-group census")
    require(int(group.group(1)) == 112, "P29 literal group size differs")
    require(int(group.group(6)) + int(group.group(7)) == int(group.group(2)), "group choices differ")
    require(int(group.group(8)) == 0, "excluded zstd10 group was selected")

    hashes = read_hashes(root / "tooling.sha256")
    binary_hashes = [
        digest
        for name, digest in hashes.items()
        if Path(name).name.startswith("codec50") and Path(name).suffix != ".cpp"
    ]
    source_hashes = [digest for name, digest in hashes.items() if Path(name).name == "codec50.cpp"]
    require(len(binary_hashes) == 1, "cannot identify one P29 binary hash")
    require(len(source_hashes) == 1, "cannot identify one P29 source hash")

    prefix_literal_raw = sum_column(prefix_components, "literal_raw_bytes")
    prefix_region_raw = sum_column(prefix_components, "region_control_raw_bytes")
    prefix_array_raw = sum_column(prefix_components, "array_values_raw_bytes")
    full_plan_time = read_time(root / "full/plan.time")
    full_grouped_time = read_time(root / "full/grouped.time")
    prefix_plan_time = read_time(root / "prefix/plan.time")
    prefix_grouped_time = read_time(root / "prefix/grouped.time")

    return {
        "project": expected["project"],
        "profile": expected["profile"],
        "tus": full_tus,
        "raw_bytes": full_raw,
        "raw_sha256": prep["raw_sha256"],
        "payload_sha256": expected["payload_sha256"],
        "stable_p29_bytes": full_wire,
        "probe_tus": prefix_tus,
        "probe_raw_bytes": identity["prefix_raw_bytes"],
        "probe_wire_bytes": prefix_wire,
        "probe_mode": expected_mode,
        "probe_literal_frames": identity["literal_frames"],
        "probe_literal_frame_sha256": identity["literal_frame_sha256"],
        "probe_literal_raw_bytes": prefix_literal_raw,
        "probe_literal_fraction": prefix_literal_raw / identity["prefix_raw_bytes"],
        "probe_region_control_raw_bytes": prefix_region_raw,
        "probe_array_values_raw_bytes": prefix_array_raw,
        "p29_c_proxy_gbps": float(endpoint.group(1)),
        "p29_f_proxy_gbps": float(endpoint.group(2)),
        "loaded_interned_s": float(loaded.group(1)),
        "full_plan_wall_s": full_plan_time["wall_s"],
        "full_grouped_wall_s": full_grouped_time["wall_s"],
        "full_research_process_wall_s": full_plan_time["wall_s"] + full_grouped_time["wall_s"],
        "probe_plan_wall_s": prefix_plan_time["wall_s"],
        "probe_grouped_wall_s": prefix_grouped_time["wall_s"],
        "probe_research_process_wall_s": prefix_plan_time["wall_s"] + prefix_grouped_time["wall_s"],
        "full_plan_max_rss_kib": full_plan_time["max_rss_kib"],
        "full_grouped_max_rss_kib": full_grouped_time["max_rss_kib"],
        "literal_groups": int(group.group(2)),
        "literal_zstd3_groups": int(group.group(6)),
        "literal_bsc_groups": int(group.group(7)),
        "p29_commit": meta["p29_commit"],
        "p29_source_sha256": source_hashes[0],
        "p29_binary_sha256": binary_hashes[0],
        "libbsc_commit": meta["libbsc_commit"],
        "zstd_version": meta["zstd_version"],
        "prefix_identity": True,
        "exact": True,
    }


def markdown_report(rows: list[dict[str, Any]], summary: dict[str, Any]) -> str:
    lines = [
        "# Corrected P29 chronological-prefix matrix",
        "",
        "This is the corrected P29 size and TU112 identity ledger. It is not a selector or a",
        "binding end-to-end rate result. The research process timings expose the current two-pass",
        "cost; the printed C/F values remain codec-internal proxies.",
        "",
        "## Closure",
        "",
        f"- Exact cells: **{summary['exact_cells']}/{summary['cells']}**.",
        f"- Prefix-identity cells: **{summary['prefix_identity_cells']}/{summary['cells']}**.",
        f"- Suffix-blind TU112 cells: **{summary['suffix_blind_cells']}**.",
        f"- Complete programs ending by TU112: **{summary['complete_program_cells']}**.",
        f"- Total raw: **{summary['raw_bytes']/1e9:.3f} GB**.",
        f"- Total stable P29: **{summary['stable_p29_bytes']/1e6:.3f} MB**.",
        f"- Frozen cell ledger SHA-256: `{summary['verified_cell_ledger_sha256']}`.",
        "",
        "## Rows",
        "",
        "| project | profile | TUs | raw GB | stable P29 MB | probe mode | probe TUs | probe MB | C/F proxy GB/s | two-pass wall s |",
        "|---|---|---:|---:|---:|:---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['project']} | {row['profile']} | {row['tus']} | "
            f"{row['raw_bytes']/1e9:.3f} | {row['stable_p29_bytes']/1e6:.3f} | "
            f"{row['probe_mode']} | {row['probe_tus']} | {row['probe_wire_bytes']/1e6:.3f} | "
            f"{row['p29_c_proxy_gbps']:.3f}/{row['p29_f_proxy_gbps']:.3f} | "
            f"{row['full_research_process_wall_s']:.3f} |"
        )
    lines.extend(
        [
            "",
            "Machine-readable evidence: `p29-prefix-measurements.tsv` and",
            "`p29-prefix-summary.json`. Every row retains complete and prefix curves, component",
            "ledgers, raw-plane prefix hashes, literal-frame hashes, source/binary hashes, and",
            "process timing files.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--expected-cells", type=int, default=44)
    args = parser.parse_args()
    root = args.run_root.resolve()

    ledger_rows = read_tsv(root / "verified-cells.tsv")
    require(len(ledger_rows) == args.expected_cells, "frozen cell ledger count differs")
    expected = {(row["project"], row["profile"]): row for row in ledger_rows}
    paths = sorted((root / "cells").glob("*/*/prefix-identity.json"))
    require(len(paths) == args.expected_cells, f"expected {args.expected_cells} runs, found {len(paths)}")

    rows: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for path in paths:
        profile = path.parent.name
        project = path.parent.parent.name
        identity = (project, profile)
        require(identity in expected, f"run is not in frozen ledger: {identity}")
        require(identity not in seen, f"duplicate run: {identity}")
        seen.add(identity)
        rows.append(parse_cell(path.parent, expected[identity]))
    require(seen == set(expected), "one or more frozen cells are missing")
    rows.sort(key=lambda row: (row["project"], row["profile"]))

    fieldnames = list(rows[0])
    with (root / "p29-prefix-measurements.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, delimiter="\t", fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    source_hashes = sorted({row["p29_source_sha256"] for row in rows})
    binary_hashes = sorted({row["p29_binary_sha256"] for row in rows})
    require(len(source_hashes) == 1 and len(binary_hashes) == 1, "P29 provenance drifts across cells")
    summary = {
        "schema": "p29-prefix-matrix-v1",
        "cells": len(rows),
        "projects": len({row["project"] for row in rows}),
        "profiles": sorted({row["profile"] for row in rows}),
        "raw_bytes": sum(row["raw_bytes"] for row in rows),
        "stable_p29_bytes": sum(row["stable_p29_bytes"] for row in rows),
        "probe_wire_bytes": sum(row["probe_wire_bytes"] for row in rows),
        "exact_cells": sum(row["exact"] for row in rows),
        "prefix_identity_cells": sum(row["prefix_identity"] for row in rows),
        "suffix_blind_cells": sum(row["probe_mode"] == "suffix-blind" for row in rows),
        "complete_program_cells": sum(row["probe_mode"] == "complete-program" for row in rows),
        "p29_source_sha256": source_hashes[0],
        "p29_binary_sha256": binary_hashes[0],
        "verified_cell_ledger_sha256": sha256_file(root / "verified-cells.tsv"),
        "all_exact": all(row["exact"] for row in rows),
        "all_prefix_identical": all(row["prefix_identity"] for row in rows),
        "timing_scope": "diagnostic two-process research pass; not resource-normalized selector",
    }
    (root / "p29-prefix-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    (root / "P29-PREFIX-MATRIX.md").write_text(markdown_report(rows, summary))
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
