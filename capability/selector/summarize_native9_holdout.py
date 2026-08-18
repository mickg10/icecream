#!/usr/bin/env python3
"""Strictly verify and score the frozen native-nine selector holdout."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any


STREAM_OVERHEAD = 108  # 72-byte frozen stream header + 36-byte end record.


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def one_row(path: Path) -> dict[str, str]:
    rows = read_tsv(path)
    if len(rows) != 1:
        raise ValueError(f"{path}: expected one measurement row, found {len(rows)}")
    return rows[0]


def rows_by_id(path: Path) -> dict[str, dict[str, str]]:
    rows = read_tsv(path)
    keyed: dict[str, dict[str, str]] = {}
    for row in rows:
        cell_id = row.get("id", "")
        if not cell_id or cell_id in keyed:
            raise ValueError(f"{path}: empty or duplicate id {cell_id!r}")
        keyed[cell_id] = row
    return keyed


def as_bool(value: str) -> bool:
    if value.lower() == "true":
        return True
    if value.lower() == "false":
        return False
    raise ValueError(f"not a boolean: {value}")


def parse_exact_hashes(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text().splitlines():
        fields = line.split(maxsplit=1)
        if len(fields) != 2 or len(fields[0]) != 64:
            raise ValueError(f"{path}: malformed hash line")
        try:
            int(fields[0], 16)
        except ValueError as error:
            raise ValueError(f"{path}: malformed hash digest") from error
        name = Path(fields[1].lstrip("* ")).name
        if name in values:
            raise ValueError(f"{path}: duplicate retained name {name}")
        values[name] = fields[0]
    return values


def verify_retained_hashes(cell: Path, names: list[str]) -> dict[str, str]:
    expected = parse_exact_hashes(cell / "exact.sha256")
    for name in names:
        retained = cell / name
        if not retained.is_file():
            raise ValueError(f"{cell}: missing retained file {name}")
        if expected.get(name) != sha256(retained):
            raise ValueError(f"{retained}: retained hash differs")
    return expected


def verify_curve(path: Path, expected_tus: int, expected_raw: int,
                 expected_wire: int) -> None:
    rows = read_tsv(path)
    if not rows:
        raise ValueError(f"{path}: empty GRZ curve")
    prior_hi = 0
    output = 0
    compressed = 0
    for expected_group, row in enumerate(rows):
        group = int(row["group"])
        lo = int(row["tu_lo"])
        hi = int(row["tu_hi"])
        if group != expected_group or lo != prior_hi or hi <= lo:
            raise ValueError(f"{path}: non-contiguous group {group}")
        prior_hi = hi
        output += int(row["out_bytes"])
        compressed += int(row["comp_bytes"])
        hist_base = int(row["hist_base"])
        hist_extent = int(row["hist_extent"])
        if min(int(row["out_bytes"]), int(row["comp_bytes"]), hist_base, hist_extent) < 0:
            raise ValueError(f"{path}: group {group} has a negative extent")
        if hist_base + hist_extent > output - int(row["out_bytes"]):
            raise ValueError(f"{path}: history extent exceeds prior output")
    if prior_hi != expected_tus or output != expected_raw:
        raise ValueError(f"{path}: TU/raw extent differs")
    if compressed + STREAM_OVERHEAD != expected_wire:
        raise ValueError(f"{path}: physical stream does not close")


def verify_p29_curve(path: Path, expected_tus: int, expected_raw: int,
                     expected_wire: int) -> dict[int, tuple[int, int]]:
    rows = read_tsv(path)
    if len(rows) != expected_tus:
        raise ValueError(
            f"{path}: expected {expected_tus} P29 curve rows, found {len(rows)}"
        )
    prior_raw = 0
    prior_wire = 0
    checkpoints: dict[int, tuple[int, int]] = {}
    for expected_tu, row in enumerate(rows, 1):
        raw = int(row["raw_bytes"])
        wire = int(row["wire_bytes"])
        cumulative_raw = int(row["cumulative_raw_bytes"])
        cumulative_wire = int(row["cumulative_wire_bytes"])
        if int(row["tu"]) != expected_tu or not as_bool(row["exact"]):
            raise ValueError(f"{path}: TU {expected_tu} is not an exact contiguous row")
        if raw < 0 or wire < 0:
            raise ValueError(f"{path}: TU {expected_tu} has a negative extent")
        if cumulative_raw != prior_raw + raw or cumulative_wire != prior_wire + wire:
            raise ValueError(f"{path}: TU {expected_tu} cumulative ledger does not close")
        prior_raw = cumulative_raw
        prior_wire = cumulative_wire
        if expected_tu in (100, 112, 200):
            checkpoints[expected_tu] = (cumulative_raw, cumulative_wire)
    if prior_raw != expected_raw or prior_wire != expected_wire:
        raise ValueError(f"{path}: final P29 raw/wire extent differs")
    return checkpoints


def verify_p29_prefix_identity(cell: Path, row: dict[str, str],
                               policy: dict[str, Any],
                               full_curve: Path) -> None:
    identity_path = cell / "prefix-identity.json"
    identity = json.loads(identity_path.read_text())
    expected_tus = int(policy["probe_tus"])
    expected = {
        "schema": "p29-prefix-identity-v1",
        "prefix_mode": "suffix-blind",
        "prefix_tus": expected_tus,
        "full_tus": int(row["tus"]),
        "prefix_raw_bytes": int(row["probe_raw_bytes"]),
        "prefix_wire_bytes": int(row["probe_wire_bytes"]),
    }
    for key, value in expected.items():
        if identity.get(key) != value:
            raise ValueError(f"{identity_path}: {key} differs from P29 ledger")
    if identity.get("curve_identical") is not True or identity.get("components_identical") is not True:
        raise ValueError(f"{identity_path}: independently produced prefix is not identical")

    expected_artifacts = (
        "full/components.tsv",
        "full/curve.tsv",
        "full/literal.wire",
        "prefix/components.tsv",
        "prefix/curve.tsv",
        "prefix/literal.wire",
    )
    artifact_hashes = identity.get("artifacts")
    if not isinstance(artifact_hashes, dict) or len(artifact_hashes) != len(expected_artifacts):
        raise ValueError(f"{identity_path}: retained artifact hash coverage differs")
    for relative in expected_artifacts:
        matches = [
            digest for name, digest in artifact_hashes.items()
            if str(name).endswith(f"/{relative}")
        ]
        retained = cell / relative
        if len(matches) != 1 or matches[0] != sha256(retained):
            raise ValueError(f"{retained}: P29 identity artifact hash differs")

    prefix_curve = cell / "prefix" / "curve.tsv"
    prefix_points = verify_p29_curve(
        prefix_curve,
        expected_tus,
        int(row["probe_raw_bytes"]),
        int(row["probe_wire_bytes"]),
    )
    full_lines = full_curve.read_bytes().splitlines()
    prefix_lines = prefix_curve.read_bytes().splitlines()
    if full_lines[:len(prefix_lines)] != prefix_lines:
        raise ValueError(f"{cell}: independent P29 prefix curve differs from full-run prefix")
    if prefix_points[expected_tus] != (
        int(row["probe_raw_bytes"]), int(row["probe_wire_bytes"])
    ):
        raise ValueError(f"{cell}: P29 prefix checkpoint differs")


def verify_ledger(grz_root: Path, p29_root: Path) -> list[dict[str, str]]:
    ledger_path = grz_root / "native9-ledger.tsv"
    final_path = grz_root / "native9-ledger.final.tsv"
    if ledger_path.read_bytes() != final_path.read_bytes():
        raise ValueError("GRZ before/after source ledgers differ")
    p29_ledger_path = p29_root / "fixed16-ledger.tsv"
    p29_final_path = p29_root / "fixed16-ledger.final.tsv"
    if p29_ledger_path.read_bytes() != p29_final_path.read_bytes():
        raise ValueError("P29 before/after source ledgers differ")
    if ledger_path.read_bytes() != p29_ledger_path.read_bytes():
        raise ValueError("GRZ and P29 source ledgers differ")
    ledger = read_tsv(ledger_path)
    p29 = read_tsv(p29_ledger_path)
    if len(ledger) != 9 or len(p29) != 9:
        raise ValueError("native holdout must contain exactly nine rows")
    keys = ("id", "name", "tu_count", "raw_bytes", "manifest_sha256")
    for left, right in zip(ledger, p29, strict=True):
        if any(left[key] != right[key] for key in keys):
            raise ValueError(f"source ledger mismatch at {left['id']}")
    return ledger


def select_p29(row: dict[str, str], policy: dict[str, Any]) -> tuple[bool, dict[str, float]]:
    probe_raw = int(row["probe_raw_bytes"])
    probe_wire = int(row["probe_wire_bytes"])
    if probe_raw <= 0 or probe_wire <= 0:
        raise ValueError(f"{row.get('id', '<unknown>')}: empty P29 probe")
    trajectory = float(row["probe_p29_trajectory"])
    literal_fraction = int(row["probe_literal_wire_bytes"]) / probe_wire
    root_missing_fraction = (
        int(row["probe_root_wire_bytes"])
        + int(row["probe_missing_request_wire_bytes"])
    ) / probe_wire
    common = trajectory <= float(policy["max_p29_trajectory"])
    high = probe_raw >= int(policy["high_raw_bytes"])
    medium = (
        probe_raw >= int(policy["low_raw_bytes"])
        and literal_fraction >= float(policy["min_p29_literal_wire_fraction"])
        and root_missing_fraction
        <= float(policy["max_p29_root_missing_wire_fraction"])
    )
    return common and (high or medium), {
        "trajectory": trajectory,
        "literal_fraction": literal_fraction,
        "root_missing_fraction": root_missing_fraction,
    }


def fmt_mb(value: int) -> str:
    amount = value / 1_000_000
    return f"{amount:.1f}" if amount < 10 else f"{amount:.0f}"


def summarize(args: argparse.Namespace) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    ledger = verify_ledger(args.grz_root, args.p29_root)
    checkpoint_ledger = args.checkpoint_root / "native9-ledger.tsv"
    if (args.grz_root / "native9-ledger.tsv").read_bytes() != checkpoint_ledger.read_bytes():
        raise ValueError("full-run and checkpoint source ledgers differ")
    policy = json.loads(args.policy.read_text())
    if policy.get("status") != "frozen-before-native9-holdout":
        raise ValueError("selector policy is not the pre-holdout frozen policy")
    if set(policy["holdout_projects"]) != {row["name"] for row in ledger}:
        raise ValueError("frozen policy holdout names differ from source ledger")

    p29_rows = rows_by_id(args.p29_root / "p29-native9-measurements.tsv")
    checkpoint_rows = rows_by_id(
        args.checkpoint_root / "native9-prefix-checkpoints.tsv"
    )
    if set(p29_rows) != {row["id"] for row in ledger} or set(checkpoint_rows) != set(p29_rows):
        raise ValueError("P29/checkpoint measurement coverage differs from ledger")

    rows: list[dict[str, Any]] = []
    for source in ledger:
        cell_id = source["id"]
        name = source["name"]
        tus = int(source["tu_count"])
        raw = int(source["raw_bytes"])
        manifest_sha = source["manifest_sha256"]

        grz_cell = args.grz_root / "cells" / cell_id
        grz = one_row(grz_cell / "measurement.tsv")
        if (
            grz["name"] != name
            or int(grz["tus"]) != tus
            or int(grz["raw_bytes"]) != raw
            or grz["manifest_sha256"] != manifest_sha
            or not all(as_bool(grz[key]) for key in ("grz_exact", "z19_exact", "z6_exact"))
        ):
            raise ValueError(f"{cell_id}: GRZ measurement/source mismatch")
        grz_wire = int(grz["grz_wire_bytes"])
        z19_wire = int(grz["z19_long_bytes"])
        z6_wire = int(grz["z6_long_bytes"])
        for field, filename in (
            ("grz_wire_bytes", "cell.grz"),
            ("z19_long_bytes", "cell.z19.zst"),
            ("z6_long_bytes", "cell.z6.zst"),
            ("tu100_z6_long_bytes", "tu100.z6.zst"),
            ("tu200_z6_long_bytes", "tu200.z6.zst"),
        ):
            if (grz_cell / filename).stat().st_size != int(grz[field]):
                raise ValueError(f"{grz_cell / filename}: size ledger differs")
        hashes = verify_retained_hashes(
            grz_cell,
            [
                "cell.tu",
                "cell.grz",
                "cell.z19.zst",
                "cell.z6.zst",
                "tu100.z6.zst",
                "tu200.z6.zst",
                "grz-curve.tsv",
            ],
        )
        if hashes.get("cell.ii") != hashes.get("replay.ii"):
            raise ValueError(f"{cell_id}: encode/decode digest pair differs")
        verify_curve(grz_cell / "grz-curve.tsv", tus, raw, grz_wire)

        p29 = p29_rows[cell_id]
        if (
            p29["name"] != name
            or int(p29["tus"]) != tus
            or int(p29["raw_bytes"]) != raw
            or p29["manifest_sha256"] != manifest_sha
            or int(p29["probe_tus"]) != int(policy["probe_tus"])
            or p29["probe_mode"] != "suffix-blind"
            or not as_bool(p29["prefix_identity"])
            or not as_bool(p29["exact"])
        ):
            raise ValueError(f"{cell_id}: P29 measurement/source mismatch")
        p29_wire = int(p29["complete_wire_bytes"])
        p29_cell = args.p29_root / "cells" / cell_id
        p29_curve = p29_cell / "full" / "curve.tsv"
        p29_points = verify_p29_curve(p29_curve, tus, raw, p29_wire)
        verify_p29_prefix_identity(p29_cell, p29, policy, p29_curve)

        checkpoint = checkpoint_rows[cell_id]
        if (
            checkpoint["name"] != name
            or int(checkpoint["tus"]) != tus
            or int(checkpoint["raw_bytes"]) != raw
            or checkpoint["manifest_sha256"] != manifest_sha
            or not as_bool(checkpoint["exact"])
        ):
            raise ValueError(f"{cell_id}: prefix checkpoint/source mismatch")
        checkpoint_cell = args.checkpoint_root / "cells" / cell_id
        for turn in (100, 200):
            prefix = checkpoint_cell / f"tu{turn}"
            prefix_raw = int(checkpoint[f"tu{turn}_raw_bytes"])
            prefix_wire = int(checkpoint[f"tu{turn}_grz_bytes"])
            prefix_hashes = parse_exact_hashes(prefix.with_suffix(".exact.sha256"))
            if prefix_hashes.get(f"tu{turn}.ii") != prefix_hashes.get(f"tu{turn}.replay.ii"):
                raise ValueError(f"{cell_id}: TU{turn} digest pair differs")
            for filename in (f"tu{turn}.tu", f"tu{turn}.grz", f"tu{turn}.curve.tsv"):
                retained = checkpoint_cell / filename
                if prefix_hashes.get(filename) != sha256(retained):
                    raise ValueError(f"{retained}: checkpoint hash differs")
            if (checkpoint_cell / f"tu{turn}.grz").stat().st_size != prefix_wire:
                raise ValueError(f"{cell_id}: TU{turn} GRZ size differs")
            verify_curve(checkpoint_cell / f"tu{turn}.curve.tsv", turn, prefix_raw, prefix_wire)
            recorded_pass = as_bool(checkpoint[f"tu{turn}_pass"])
            if recorded_pass != (prefix_wire <= int(checkpoint[f"tu{turn}_z6_long_bytes"])):
                raise ValueError(f"{cell_id}: TU{turn} recorded result differs")

        use_p29, features = select_p29(p29, policy)
        selected_codec = "P29" if use_p29 else "GRZ"
        selected_wire = p29_wire if use_p29 else grz_wire
        hindsight_wire = min(p29_wire, grz_wire)
        prefix_values: dict[int, tuple[int, int, int]] = {}
        for turn in (100, 200):
            z6_prefix = int(checkpoint[f"tu{turn}_z6_long_bytes"])
            expected_raw = int(checkpoint[f"tu{turn}_raw_bytes"])
            if use_p29:
                p29_raw, selected_prefix = p29_points[turn]
                if p29_raw != expected_raw:
                    raise ValueError(f"{cell_id}: P29/GRZ TU{turn} raw extent differs")
            else:
                selected_prefix = int(checkpoint[f"tu{turn}_grz_bytes"])
            prefix_values[turn] = (expected_raw, selected_prefix, z6_prefix)

        rows.append(
            {
                "id": cell_id,
                "name": name,
                "tus": tus,
                "raw_bytes": raw,
                "p29_wire_bytes": p29_wire,
                "grz_wire_bytes": grz_wire,
                "selected_codec": selected_codec,
                "selected_wire_bytes": selected_wire,
                "hindsight_wire_bytes": hindsight_wire,
                "regret_bytes": selected_wire - hindsight_wire,
                "z19_long_bytes": z19_wire,
                "z6_long_bytes": z6_wire,
                "cold_110pct_z19_pass": selected_wire * 10 <= z19_wire * 11,
                "tu100_raw_bytes": prefix_values[100][0],
                "tu100_selected_bytes": prefix_values[100][1],
                "tu100_z6_long_bytes": prefix_values[100][2],
                "tu100_z6_pass": prefix_values[100][1] <= prefix_values[100][2],
                "tu200_raw_bytes": prefix_values[200][0],
                "tu200_selected_bytes": prefix_values[200][1],
                "tu200_z6_long_bytes": prefix_values[200][2],
                "tu200_z6_pass": prefix_values[200][1] <= prefix_values[200][2],
                **features,
            }
        )

    totals = {
        "schema": "frozen-selector-native9-holdout-v1",
        "cells": len(rows),
        "raw_bytes": sum(row["raw_bytes"] for row in rows),
        "p29_wire_bytes": sum(row["p29_wire_bytes"] for row in rows),
        "grz_wire_bytes": sum(row["grz_wire_bytes"] for row in rows),
        "selected_wire_bytes": sum(row["selected_wire_bytes"] for row in rows),
        "hindsight_wire_bytes": sum(row["hindsight_wire_bytes"] for row in rows),
        "z19_long_bytes": sum(row["z19_long_bytes"] for row in rows),
        "z6_long_bytes": sum(row["z6_long_bytes"] for row in rows),
        "tu100_selected_bytes": sum(row["tu100_selected_bytes"] for row in rows),
        "tu100_z6_long_bytes": sum(row["tu100_z6_long_bytes"] for row in rows),
        "tu200_selected_bytes": sum(row["tu200_selected_bytes"] for row in rows),
        "tu200_z6_long_bytes": sum(row["tu200_z6_long_bytes"] for row in rows),
        "cold_pass_cells": sum(row["cold_110pct_z19_pass"] for row in rows),
        "tu100_pass_cells": sum(row["tu100_z6_pass"] for row in rows),
        "tu200_pass_cells": sum(row["tu200_z6_pass"] for row in rows),
        "selector_correct_cells": sum(row["regret_bytes"] == 0 for row in rows),
        "policy_sha256": sha256(args.policy),
        "grz_ledger_sha256": sha256(args.grz_root / "native9-ledger.tsv"),
    }
    totals["regret_bytes"] = totals["selected_wire_bytes"] - totals["hindsight_wire_bytes"]
    totals["cold_total_110pct_z19_pass"] = totals["selected_wire_bytes"] * 10 <= totals["z19_long_bytes"] * 11
    totals["tu100_total_z6_pass"] = totals["tu100_selected_bytes"] <= totals["tu100_z6_long_bytes"]
    totals["tu200_total_z6_pass"] = totals["tu200_selected_bytes"] <= totals["tu200_z6_long_bytes"]
    return rows, totals


def write_outputs(output: Path, rows: list[dict[str, Any]], totals: dict[str, Any]) -> None:
    output.mkdir(parents=True, exist_ok=False)
    table = output / "native9-selector-holdout.tsv"
    with table.open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    (output / "native9-selector-holdout.json").write_text(
        json.dumps({"summary": totals, "rows": rows}, indent=2, sort_keys=True) + "\n"
    )
    lines = [
        "# Frozen selector native-nine holdout",
        "",
        "The selector policy was frozen before these nine labels were read. GRZ TU100/TU200 values are complete independently decoded prefix streams; no buffered 112-TU group is counted as zero wire.",
        "",
        "| Project | Choice | Cold MB | z19-long MB | Cold gate | TU100 MB / z6 | TU200 MB / z6 | Regret MB |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['name']} | {row['selected_codec']} | {fmt_mb(row['selected_wire_bytes'])} | "
            f"{fmt_mb(row['z19_long_bytes'])} | {'PASS' if row['cold_110pct_z19_pass'] else 'FAIL'} | "
            f"{fmt_mb(row['tu100_selected_bytes'])} / {fmt_mb(row['tu100_z6_long_bytes'])} | "
            f"{fmt_mb(row['tu200_selected_bytes'])} / {fmt_mb(row['tu200_z6_long_bytes'])} | "
            f"{fmt_mb(row['regret_bytes'])} |"
        )
    lines.extend(
        [
            "",
            f"Selected total: **{totals['selected_wire_bytes']:,} B**; z19-long total: **{totals['z19_long_bytes']:,} B**; hindsight: **{totals['hindsight_wire_bytes']:,} B**; regret: **{totals['regret_bytes']:,} B**.",
            "",
            f"Cold cells: **{totals['cold_pass_cells']}/9**; TU100: **{totals['tu100_pass_cells']}/9**; TU200: **{totals['tu200_pass_cells']}/9**; exact selector choices: **{totals['selector_correct_cells']}/9**.",
            "",
            f"Aggregate TU100: **{totals['tu100_selected_bytes']:,} B / {totals['tu100_z6_long_bytes']:,} B** ({'PASS' if totals['tu100_total_z6_pass'] else 'FAIL'}); aggregate TU200: **{totals['tu200_selected_bytes']:,} B / {totals['tu200_z6_long_bytes']:,} B** ({'PASS' if totals['tu200_total_z6_pass'] else 'FAIL'}).",
        ]
    )
    (output / "NATIVE9-SELECTOR-HOLDOUT.md").write_text("\n".join(lines) + "\n")
    retained = sorted(path for path in output.iterdir() if path.name != "SHA256SUMS")
    (output / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in retained)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--grz-root", type=Path, required=True)
    parser.add_argument("--checkpoint-root", type=Path, required=True)
    parser.add_argument("--p29-root", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows, totals = summarize(args)
    write_outputs(args.output, rows, totals)
    print(json.dumps(totals, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
