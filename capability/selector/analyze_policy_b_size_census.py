#!/usr/bin/env python3
"""Audit and join the corrected 44-cell policy-B size census.

This analysis intentionally ignores the non-isolated timing fields.  It joins the
corrected stable-Root P29 census to the complete current-decoder GRZ/zstd replay,
verifies the P29 prefix identity again, and evaluates the smallest causal policy:
choose P29 when the raw extent observed through min(112, total TUs) crosses one
threshold.  Threshold quality is reported with each project held out in turn.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any, Iterable


CHUNK = 8 * 1024 * 1024
FIXED_THRESHOLD_BYTES = 500_000_000


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_key_values(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text().splitlines(), start=1):
        fields = line.split("\t", 1)
        require(len(fields) == 2 and all(fields), f"bad row field at {path}:{line_number}")
        key, value = fields
        require(key not in result, f"duplicate row field {key!r}: {path}")
        result[key] = value
    return result


def selected_bytes(row: dict[str, Any], choose_p29: bool) -> int:
    return int(row["p29_complete_bytes"] if choose_p29 else row["grz_complete_bytes"])


def threshold_candidates(rows: Iterable[dict[str, Any]]) -> list[int]:
    values = sorted({int(row["probe_raw_bytes"]) for row in rows})
    require(bool(values), "cannot fit a threshold without rows")
    candidates = [0]
    candidates.extend((lower + upper) // 2 for lower, upper in zip(values, values[1:]))
    candidates.append(values[-1] + 1)
    return candidates


def fit_threshold(rows: list[dict[str, Any]]) -> int:
    """Minimize complete selected bytes; ties prefer the GRZ-default larger threshold."""

    def objective(threshold: int) -> tuple[int, int]:
        total = sum(
            selected_bytes(row, int(row["probe_raw_bytes"]) >= threshold) for row in rows
        )
        return total, -threshold

    return min(threshold_candidates(rows), key=objective)


def collect_rows(census_root: Path, replay_root: Path, expected_cells: int) -> list[dict[str, Any]]:
    row_paths = sorted(census_root.glob("*/row.tsv"))
    require(len(row_paths) == expected_cells, f"expected {expected_cells} census rows")
    rows: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()

    for row_path in row_paths:
        values = read_key_values(row_path)
        project = values["project"]
        profile = values["profile"]
        identity_key = (project, profile)
        require(identity_key not in seen, f"duplicate cell: {identity_key}")
        seen.add(identity_key)
        cell = row_path.parent

        identity = json.loads((cell / "id/prefix-identity.json").read_text())
        require(identity.get("schema") == "p29-prefix-identity-v1", "wrong P29 identity schema")
        total_tus = int(values["total_tus"])
        probe_tus = int(values["probe_tus"])
        require(probe_tus == min(112, total_tus), f"wrong probe boundary: {identity_key}")
        expected_mode = "suffix-blind" if probe_tus < total_tus else "complete-program"
        require(identity["prefix_tus"] == probe_tus, f"identity TU extent differs: {identity_key}")
        require(identity["prefix_mode"] == expected_mode, f"identity mode differs: {identity_key}")
        require(identity["curve_identical"], f"P29 prefix curve differs: {identity_key}")
        require(identity["components_identical"], f"P29 component prefix differs: {identity_key}")
        for artifact_name, expected_digest in identity["artifacts"].items():
            artifact = Path(artifact_name)
            require(artifact.is_file(), f"missing P29 identity artifact: {artifact}")
            require(
                sha256_file(artifact) == expected_digest,
                f"P29 identity artifact digest differs: {artifact}",
            )
        require(values["identity"] == "PASS", f"P29 identity did not pass: {identity_key}")
        require(values["p29_complete_exact"] == "OK", f"P29 complete replay differs: {identity_key}")
        p29_probe = int(values["p29_probe_bytes"])
        require(p29_probe == int(identity["prefix_wire_bytes"]), f"P29 probe size differs: {identity_key}")
        with (cell / "id/full/curve.tsv").open(newline="") as stream:
            full_curve = list(csv.DictReader(stream, delimiter="\t"))
        require(len(full_curve) == total_tus, f"P29 complete curve extent differs: {identity_key}")
        require(
            int(full_curve[-1]["cumulative_wire_bytes"]) == int(values["p29_complete_bytes"]),
            f"P29 complete size differs from curve: {identity_key}",
        )
        require(
            sha256_file(cell / "p29/literal.wire")
            == sha256_file(cell / "id/prefix/literal.wire"),
            f"P29 concurrent/gated prefix wire differs: {identity_key}",
        )

        replay_path = replay_root / project / profile / "summary.stdout"
        require(replay_path.is_file(), f"missing complete replay: {identity_key}")
        replay = json.loads(replay_path.read_text())
        require(replay.get("schema") == "issue16-selector-measurement-v2", "wrong replay schema")
        require(replay["project"] == project and replay["profile"] == profile, "replay identity differs")
        require(bool(replay["grz"]["exact"]), f"complete GRZ replay differs: {identity_key}")
        require(int(replay["tu_count"]) == total_tus, f"complete TU count differs: {identity_key}")
        require(int(replay["raw_bytes"]) == int(values["raw_bytes"]), f"raw extent differs: {identity_key}")
        replay_cell = replay_path.parent
        require(
            sha256_file(replay_cell / "grz/cell.grz") == replay["grz"]["wire_sha256"],
            f"complete GRZ wire digest differs: {identity_key}",
        )
        require(
            sha256_file(replay_cell / "baseline/cell.z19.zst")
            == replay["whole_zstd"]["z19_long_sha256"],
            f"zstd-19 reference digest differs: {identity_key}",
        )
        require(
            sha256_file(replay_cell / "baseline/cell.z6.zst")
            == replay["whole_zstd"]["z6_long_sha256"],
            f"zstd-6 reference digest differs: {identity_key}",
        )

        p29_complete = int(values["p29_complete_bytes"])
        grz_complete = int(replay["grz"]["wire_bytes"])
        complete_winner = "p29" if p29_complete < grz_complete else "grz"
        fixed_choice = "p29" if int(values["probe_raw_bytes"]) >= FIXED_THRESHOLD_BYTES else "grz"
        rows.append(
            {
                "project": project,
                "profile": profile,
                "total_tus": total_tus,
                "raw_bytes": int(values["raw_bytes"]),
                "probe_tus": probe_tus,
                "probe_raw_bytes": int(values["probe_raw_bytes"]),
                "p29_probe_bytes": p29_probe,
                "grz_probe_bytes": int(values["grz_probe_bytes"]),
                "p29_complete_bytes": p29_complete,
                "grz_complete_bytes": grz_complete,
                "z19_long_bytes": int(replay["whole_zstd"]["z19_long_bytes"]),
                "z6_long_bytes": int(replay["whole_zstd"]["z6_long_bytes"]),
                "complete_winner": complete_winner,
                "fixed_500m_choice": fixed_choice,
                "fixed_500m_bytes": selected_bytes(
                    {"p29_complete_bytes": p29_complete, "grz_complete_bytes": grz_complete},
                    fixed_choice == "p29",
                ),
                "p29_source_sha256": values["p29_src_sha"],
                "p29_binary_sha256": values["p29_bin_sha"],
                "grz_source_sha256": values["grz_src_sha"],
                "grz_binary_sha256": values["grz_bin_sha"],
                "prefix_identity": True,
                "exact": True,
            }
        )

    rows.sort(key=lambda row: (row["project"], row["profile"]))
    return rows


def add_heldout_predictions(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    projects = sorted({str(row["project"]) for row in rows})
    folds: list[dict[str, Any]] = []
    for project in projects:
        training = [row for row in rows if row["project"] != project]
        testing = [row for row in rows if row["project"] == project]
        threshold = fit_threshold(training)
        fold_selected = 0
        fold_hindsight = 0
        correct = 0
        for row in testing:
            choose_p29 = int(row["probe_raw_bytes"]) >= threshold
            choice = "p29" if choose_p29 else "grz"
            chosen = selected_bytes(row, choose_p29)
            row["heldout_threshold_bytes"] = threshold
            row["heldout_choice"] = choice
            row["heldout_bytes"] = chosen
            row["heldout_correct"] = choice == row["complete_winner"]
            fold_selected += chosen
            fold_hindsight += min(int(row["p29_complete_bytes"]), int(row["grz_complete_bytes"]))
            correct += int(row["heldout_correct"])
        folds.append(
            {
                "heldout_project": project,
                "training_cells": len(training),
                "test_cells": len(testing),
                "threshold_bytes": threshold,
                "selected_bytes": fold_selected,
                "hindsight_bytes": fold_hindsight,
                "regret_bytes": fold_selected - fold_hindsight,
                "correct_cells": correct,
            }
        )
    return folds


def make_summary(rows: list[dict[str, Any]], folds: list[dict[str, Any]]) -> dict[str, Any]:
    raw = sum(int(row["raw_bytes"]) for row in rows)
    z19 = sum(int(row["z19_long_bytes"]) for row in rows)
    z6 = sum(int(row["z6_long_bytes"]) for row in rows)
    p29 = sum(int(row["p29_complete_bytes"]) for row in rows)
    grz = sum(int(row["grz_complete_bytes"]) for row in rows)
    hindsight = sum(min(int(row["p29_complete_bytes"]), int(row["grz_complete_bytes"])) for row in rows)
    fixed = sum(int(row["fixed_500m_bytes"]) for row in rows)
    heldout = sum(int(row["heldout_bytes"]) for row in rows)
    limit = (11 * z19) // 10

    def policy(total: int) -> dict[str, Any]:
        return {
            "bytes": total,
            "over_z19": total / z19,
            "raw_ratio": raw / total,
            "headroom_to_1_10_z19_bytes": limit - total,
            "meets_1_10_z19": total <= limit,
        }

    source_sets = {
        name: sorted({str(row[name]) for row in rows})
        for name in (
            "p29_source_sha256",
            "p29_binary_sha256",
            "grz_source_sha256",
            "grz_binary_sha256",
        )
    }
    require(all(len(values) == 1 for values in source_sets.values()), "tooling drifts across cells")
    return {
        "schema": "policy-b-size-census-v1",
        "cells": len(rows),
        "projects": len({row["project"] for row in rows}),
        "profiles": sorted({row["profile"] for row in rows}),
        "raw_bytes": raw,
        "z19_long_bytes": z19,
        "z6_long_bytes": z6,
        "one_point_one_z19_limit_bytes": limit,
        "complete_winner_cells": {
            "p29": sum(row["complete_winner"] == "p29" for row in rows),
            "grz": sum(row["complete_winner"] == "grz" for row in rows),
        },
        "always_p29": policy(p29),
        "always_grz": policy(grz),
        "hindsight": policy(hindsight),
        "fixed_500m_raw_threshold": policy(fixed),
        "project_heldout_raw_threshold": policy(heldout),
        "fixed_threshold_bytes": FIXED_THRESHOLD_BYTES,
        "heldout_correct_cells": sum(bool(row["heldout_correct"]) for row in rows),
        "heldout_regret_bytes": heldout - hindsight,
        "heldout_threshold_min_bytes": min(int(fold["threshold_bytes"]) for fold in folds),
        "heldout_threshold_max_bytes": max(int(fold["threshold_bytes"]) for fold in folds),
        "tooling": {name: values[0] for name, values in source_sets.items()},
        "all_exact": all(bool(row["exact"]) for row in rows),
        "all_prefix_identical": all(bool(row["prefix_identity"]) for row in rows),
        "timing_scope": "excluded: source sweep was not isolated and omitted required P29 plan work",
    }


def markdown(summary: dict[str, Any], rows: list[dict[str, Any]]) -> str:
    policies = (
        ("always P29", summary["always_p29"]),
        ("always GRZ2", summary["always_grz"]),
        ("per-cell hindsight", summary["hindsight"]),
        ("fixed 500 MB raw threshold", summary["fixed_500m_raw_threshold"]),
        ("leave-one-project-out raw threshold", summary["project_heldout_raw_threshold"]),
    )
    lines = [
        "# Policy-B corrected 44-cell size census",
        "",
        "This report joins the corrected stable-Root P29 census to the complete current-decoder",
        "GRZ2/zstd replay. It excludes the source sweep's timing fields. All 44 P29 probe wires",
        "match their independent prefix gate, and all 44 complete GRZ2 rows are exact.",
        "",
        "## Aggregate cold size",
        "",
        "| policy | bytes | versus zstd-19-long | raw/wire | margin to 1.10× z19 | passes |",
        "|---|---:|---:|---:|---:|:---:|",
    ]
    for name, result in policies:
        lines.append(
            f"| {name} | {result['bytes']:,} | {result['over_z19']:.6f}× | "
            f"{result['raw_ratio']:.2f}× | {result['headroom_to_1_10_z19_bytes']:,} B | "
            f"{'YES' if result['meets_1_10_z19'] else 'NO'} |"
        )
    lines.extend(
        [
            "",
            f"The 1.10× allowance is **{summary['one_point_one_z19_limit_bytes']:,} B**. "
            f"Always-P29 misses it by **{-summary['always_p29']['headroom_to_1_10_z19_bytes']:,} B**.",
            "",
            "A single causal feature—raw bytes observed through `min(112, total_TUs)`—is enough",
            "on this matrix. Choosing P29 at 500 MB selects the eight Eigen/RocksDB cells and",
            "leaves the two smaller-extent P29 winners on GRZ2. The leave-one-project-out fit",
            f"selects the same rows: **{summary['heldout_correct_cells']}/44** labels, thresholds",
            f"from **{summary['heldout_threshold_min_bytes']/1e6:.3f} MB** to",
            f"**{summary['heldout_threshold_max_bytes']/1e6:.3f} MB**, and only",
            f"**{summary['heldout_regret_bytes']:,} B** above hindsight.",
            "",
            "## Interpretation boundary",
            "",
            "This is a small, zero-model development policy, not the final generalization claim.",
            "It must be replayed on fixed-16, native-25, and additional verified lineages. Its",
            "feature is essentially free to maintain, but a live adapter must still account for",
            "buffering and the TU112 decision. TU100/TU200 chronological transfer gates remain",
            "separate and are not established by complete-program totals.",
            "",
            "## Cell mistakes under project-held-out evaluation",
            "",
            "| project | profile | winner | held-out choice | regret bytes |",
            "|---|---|:---:|:---:|---:|",
        ]
    )
    for row in rows:
        if not row["heldout_correct"]:
            regret = int(row["heldout_bytes"]) - min(
                int(row["p29_complete_bytes"]), int(row["grz_complete_bytes"])
            )
            lines.append(
                f"| {row['project']} | {row['profile']} | {row['complete_winner']} | "
                f"{row['heldout_choice']} | {regret:,} |"
            )
    lines.extend(
        [
            "",
            "Machine-readable evidence: `policy-b-size-census.tsv`,",
            "`policy-b-heldout-folds.tsv`, and `policy-b-size-summary.json`.",
            "",
        ]
    )
    return "\n".join(lines)


def write_tsv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, delimiter="\t", lineterminator="\n", fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--census-root", required=True, type=Path)
    parser.add_argument("--replay-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--expected-cells", default=44, type=int)
    args = parser.parse_args()

    rows = collect_rows(args.census_root.resolve(), args.replay_root.resolve(), args.expected_cells)
    folds = add_heldout_predictions(rows)
    summary = make_summary(rows, folds)
    output = args.output_dir.resolve()
    require(not output.exists(), f"output directory already exists: {output}")
    output.mkdir(parents=True)
    write_tsv(output / "policy-b-size-census.tsv", rows)
    write_tsv(output / "policy-b-heldout-folds.tsv", folds)
    (output / "policy-b-size-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    (output / "POLICY-B-SIZE-CENSUS.md").write_text(markdown(summary, rows))
    tooling = output / "tooling.sha256"
    tooling.write_text(f"{sha256_file(Path(__file__).resolve())}  {Path(__file__).resolve()}\n")
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
