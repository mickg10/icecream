#!/usr/bin/env python3
"""Freeze and audit the minimal representation-shaped selector candidate.

The rule is intentionally tiny and contains no project, profile, timing, or
future-build field.  This script joins the audited 44-cell development matrix
to the separately replayed fixed-16 native matrix, verifies their identities,
recomputes every prediction, and writes the immutable pre-holdout policy.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any, Iterable


CHUNK = 8 * 1024 * 1024
HIGH_RAW_BYTES = 500_000_000
LOW_RAW_BYTES = 200_000_000
MAX_TRAJECTORY = 0.30
MIN_LITERAL_WIRE_FRACTION = 0.57
MAX_ROOT_MISSING_WIRE_FRACTION = 0.05
HOLDOUT_PROJECTS = [
    "gcc",
    "firefox",
    "qt6",
    "clickhouse",
    "pytorch",
    "folly",
    "arrow",
    "bitcoin",
    "v8",
]


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


def as_bool(value: str) -> bool:
    require(value in {"True", "true", "YES"}, f"expected true value, got {value!r}")
    return True


def choose_p29(row: dict[str, Any]) -> bool:
    raw = int(row["probe_raw_bytes"])
    trajectory = float(row["p29_probe_trajectory"])
    literal = float(row["p29_probe_literal_wire_fraction"])
    root_missing = float(row["p29_probe_root_missing_wire_fraction"])
    mature_large = raw >= HIGH_RAW_BYTES and trajectory <= MAX_TRAJECTORY
    literal_shaped = (
        raw >= LOW_RAW_BYTES
        and literal >= MIN_LITERAL_WIRE_FRACTION
        and root_missing <= MAX_ROOT_MISSING_WIRE_FRACTION
        and trajectory <= MAX_TRAJECTORY
    )
    return mature_large or literal_shaped


def normalize_development(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for source in read_tsv(path):
        require(as_bool(source["exact"]) and as_bool(source["prefix_identity"]), "development row is not exact")
        row: dict[str, Any] = dict(source)
        row.update(
            {
                "dataset": "matrix44",
                "generation": source["profile"],
                "p29_probe_root_missing_wire_fraction": float(
                    source["p29_probe_root_wire_fraction"]
                )
                + float(source["p29_probe_missing_request_wire_fraction"]),
            }
        )
        rows.append(row)
    require(len(rows) == 44, "expected 44 development rows")
    require(len({(row["project"], row["generation"]) for row in rows}) == 44, "duplicate development identity")
    return rows


def normalize_fixed(
    p29_path: Path, grz_probe_path: Path, grz_complete_path: Path
) -> list[dict[str, Any]]:
    p29_rows = read_tsv(p29_path)
    probe_rows = {row["id"]: row for row in read_tsv(grz_probe_path)}
    complete_rows = {row["id"]: row for row in read_tsv(grz_complete_path)}
    require(len(p29_rows) == len(probe_rows) == len(complete_rows) == 16, "fixed-16 extent differs")
    rows: list[dict[str, Any]] = []
    for p29 in p29_rows:
        identity = p29["id"]
        require(identity in probe_rows and identity in complete_rows, f"fixed-16 identity differs: {identity}")
        probe = probe_rows[identity]
        complete = complete_rows[identity]
        require(as_bool(p29["exact"]) and as_bool(p29["prefix_identity"]), f"P29 is not exact: {identity}")
        require(as_bool(probe["exact"]) and as_bool(complete["exact"]), f"GRZ is not exact: {identity}")
        require(
            p29["manifest_sha256"] == probe["manifest_sha256"]
            and p29["probe_tus"] == probe["probe_tus"]
            and p29["probe_raw_bytes"] == probe["probe_raw_bytes"],
            f"fixed-16 probe identity differs: {identity}",
        )
        require(p29["raw_bytes"] == complete["raw"], f"fixed-16 complete raw differs: {identity}")
        p29_wire = int(p29["probe_wire_bytes"])
        p29_complete = int(p29["complete_wire_bytes"])
        grz_complete = int(complete["out"])
        row: dict[str, Any] = {
            "dataset": "fixed16",
            "project": p29["name"].lower(),
            "generation": "native-fixed16",
            "profile": "native-fixed16",
            "total_tus": int(p29["tus"]),
            "raw_bytes": int(p29["raw_bytes"]),
            "probe_tus": int(p29["probe_tus"]),
            "probe_raw_bytes": int(p29["probe_raw_bytes"]),
            "p29_probe_bytes": p29_wire,
            "grz_probe_bytes": int(probe["probe_wire_bytes"]),
            "p29_complete_bytes": p29_complete,
            "grz_complete_bytes": grz_complete,
            "z19_long_bytes": int(complete["wp_z19"]),
            "complete_winner": "p29" if p29_complete < grz_complete else "grz",
            "p29_probe_trajectory": float(p29["probe_p29_trajectory"]),
            "p29_probe_literal_wire_fraction": int(p29["probe_literal_wire_bytes"])
            / p29_wire,
            "p29_probe_root_missing_wire_fraction": (
                int(p29["probe_root_wire_bytes"])
                + int(p29["probe_missing_request_wire_bytes"])
            )
            / p29_wire,
            "grz_probe_add_bpr": float(probe["probe_add_bpr"]),
            "grz_probe_anchor_match_fraction": float(probe["probe_anchor_match_fraction"]),
            "exact": True,
            "prefix_identity": True,
        }
        rows.append(row)
    return rows


def select_bytes(row: dict[str, Any], p29: bool) -> int:
    return int(row["p29_complete_bytes"] if p29 else row["grz_complete_bytes"])


def score(rows: Iterable[dict[str, Any]]) -> dict[str, Any]:
    material = list(rows)
    selected = sum(select_bytes(row, choose_p29(row)) for row in material)
    hindsight = sum(
        min(int(row["p29_complete_bytes"]), int(row["grz_complete_bytes"])) for row in material
    )
    z19 = sum(int(row["z19_long_bytes"]) for row in material)
    raw = sum(int(row["raw_bytes"]) for row in material)
    mistakes = [row for row in material if choose_p29(row) != (row["complete_winner"] == "p29")]
    return {
        "rows": len(material),
        "raw_bytes": raw,
        "z19_long_bytes": z19,
        "selected_bytes": selected,
        "hindsight_bytes": hindsight,
        "regret_bytes": selected - hindsight,
        "selected_over_z19": selected / z19,
        "raw_over_selected": raw / selected,
        "correct_rows": len(material) - len(mistakes),
        "mistake_rows": len(mistakes),
        "meets_1_10_z19": 10 * selected <= 11 * z19,
        "meets_400x_raw": raw >= 400 * selected,
    }


def prediction_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for row in rows:
        p29 = choose_p29(row)
        chosen = "p29" if p29 else "grz"
        selected = select_bytes(row, p29)
        hindsight = min(int(row["p29_complete_bytes"]), int(row["grz_complete_bytes"]))
        output.append(
            {
                "dataset": row["dataset"],
                "project": row["project"],
                "generation": row["generation"],
                "probe_tus": row["probe_tus"],
                "probe_raw_bytes": row["probe_raw_bytes"],
                "p29_probe_trajectory": row["p29_probe_trajectory"],
                "p29_probe_literal_wire_fraction": row["p29_probe_literal_wire_fraction"],
                "p29_probe_root_missing_wire_fraction": row[
                    "p29_probe_root_missing_wire_fraction"
                ],
                "choice": chosen,
                "complete_winner": row["complete_winner"],
                "p29_complete_bytes": row["p29_complete_bytes"],
                "grz_complete_bytes": row["grz_complete_bytes"],
                "selected_bytes": selected,
                "regret_bytes": selected - hindsight,
                "correct": chosen == row["complete_winner"],
            }
        )
    return output


def markdown(predictions: list[dict[str, Any]], summary: dict[str, Any]) -> str:
    lines = [
        "# Pre-holdout causal selector candidate",
        "",
        "The thresholds in this report are frozen before replaying native corpora 17-25.",
        "No project name, profile, timing, complete-size, or future-TU field is an input.",
        "",
        "```text",
        "P29 when",
        f"  (probe_raw >= {HIGH_RAW_BYTES:,} and trajectory <= {MAX_TRAJECTORY:.2f})",
        "or",
        f"  (probe_raw >= {LOW_RAW_BYTES:,}",
        f"   and literal/wire >= {MIN_LITERAL_WIRE_FRACTION:.2f}",
        f"   and (Root + missing-request)/wire <= {MAX_ROOT_MISSING_WIRE_FRACTION:.2f}",
        f"   and trajectory <= {MAX_TRAJECTORY:.2f})",
        "else GRZ",
        "```",
        "",
        "| data | selected B | hindsight B | regret B | selected/z19 | raw/selected | correct |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for name in ("matrix44", "fixed16", "combined"):
        item = summary[name]
        lines.append(
            f"| {name} | {item['selected_bytes']:,} | {item['hindsight_bytes']:,} | "
            f"{item['regret_bytes']:,} | {item['selected_over_z19']:.6f}x | "
            f"{item['raw_over_selected']:.3f}x | {item['correct_rows']}/{item['rows']} |"
        )
    misses = [row for row in predictions if not row["correct"]]
    lines.extend(
        [
            "",
            "## Development mistakes",
            "",
            "| data | project | generation | choice | winner | regret B |",
            "|---|---|---|:---:|:---:|---:|",
        ]
    )
    for row in misses:
        lines.append(
            f"| {row['dataset']} | {row['project']} | {row['generation']} | "
            f"{row['choice']} | {row['complete_winner']} | {row['regret_bytes']:,} |"
        )
    lines.extend(
        [
            "",
            "The fixed-16 result reaches the separate 400x score exactly because it chooses",
            "P29 for LLVM, Godot, and Eigen and current GRZ for every other native row.",
            "The nine named native holdouts are the next result; these thresholds must not be",
            "retuned after their labels are observed.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--development", required=True, type=Path)
    parser.add_argument("--fixed-p29", required=True, type=Path)
    parser.add_argument("--fixed-grz-probe", required=True, type=Path)
    parser.add_argument("--fixed-grz-complete", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    development = normalize_development(args.development)
    fixed = normalize_fixed(args.fixed_p29, args.fixed_grz_probe, args.fixed_grz_complete)
    require(not ({row["project"] for row in development + fixed} & set(HOLDOUT_PROJECTS)), "holdout leaked into development data")
    rows = development + fixed
    predictions = prediction_rows(rows)
    summary = {
        "schema": "causal-selector-development-v1",
        "matrix44": score(development),
        "fixed16": score(fixed),
        "combined": score(rows),
        "input_sha256": {
            "development": sha256_file(args.development),
            "fixed_p29": sha256_file(args.fixed_p29),
            "fixed_grz_probe": sha256_file(args.fixed_grz_probe),
            "fixed_grz_complete": sha256_file(args.fixed_grz_complete),
        },
    }
    policy = {
        "schema": "causal-selector-policy-v1",
        "probe_tus": 112,
        "high_raw_bytes": HIGH_RAW_BYTES,
        "low_raw_bytes": LOW_RAW_BYTES,
        "max_p29_trajectory": MAX_TRAJECTORY,
        "min_p29_literal_wire_fraction": MIN_LITERAL_WIRE_FRACTION,
        "max_p29_root_missing_wire_fraction": MAX_ROOT_MISSING_WIRE_FRACTION,
        "holdout_projects": HOLDOUT_PROJECTS,
        "development_input_sha256": summary["input_sha256"],
        "status": "frozen-before-native9-holdout",
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "causal-selector-development.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(
            stream, delimiter="\t", lineterminator="\n", fieldnames=list(predictions[0])
        )
        writer.writeheader()
        writer.writerows(predictions)
    (args.output_dir / "causal-selector-development-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    (args.output_dir / "causal-selector-policy-v1.json").write_text(
        json.dumps(policy, indent=2, sort_keys=True) + "\n"
    )
    (args.output_dir / "CAUSAL-SELECTOR-PRE-HOLDOUT.md").write_text(
        markdown(predictions, summary)
    )
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
