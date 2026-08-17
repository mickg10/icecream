#!/usr/bin/env python3
"""Render one project/schema row with requested complete-codec TU columns."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from run_complete_tu_matrix import CORPORA, SCHEMAS, sha256


DEFAULT_CHECKPOINTS = (50, 100, 150, 200, 250, 300)
ONE_GBIT_BYTES_PER_SECOND = 125_000_000
DECIMAL_MB = 1_000_000


def load_curve(path: Path) -> list[dict]:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    if not rows:
        raise ValueError(f"empty curve: {path}")
    for ordinal, row in enumerate(rows, 1):
        if int(row["tu"]) != ordinal or row["exact"] != "true":
            raise ValueError(f"{path}: invalid row {ordinal}")
    return rows


def historical_endpoints(repository: Path) -> dict[tuple[str, str, str], int]:
    specifications = (
        (
            "p25",
            repository / "linecache/ml-artifacts/half-cold-p25-16corpus-summary.json",
            {"cold": "keymap_cold_wire_bytes", "bit0": "bit0_wire_bytes", "bit1": "bit1_wire_bytes"},
        ),
        (
            "p26",
            repository / "linecache/ml-artifacts/mo-factor-p27-16corpus-summary.json",
            {
                "cold": "cold_p26_wire_bytes",
                "bit0": "bit0_p26_wire_bytes",
                "bit1": "bit1_p26_wire_bytes",
            },
        ),
        (
            "p27",
            repository / "linecache/ml-artifacts/mo-factor-p27-16corpus-summary.json",
            {"cold": "cold_p27_wire_bytes", "bit0": "bit0_p27_wire_bytes", "bit1": "bit1_p27_wire_bytes"},
        ),
        (
            "p28",
            repository / "linecache/ml-artifacts/s1-p28-16corpus-summary.json",
            {"cold": "cold_p28_wire_bytes", "bit0": "bit0_p28_wire_bytes", "bit1": "bit1_p28_wire_bytes"},
        ),
        (
            "p29",
            repository / "linecache/ml-artifacts/blob-mt-p29-16corpus-summary.json",
            {"cold": "cold_p29_wire_bytes", "bit0": "bit0_p29_wire_bytes", "bit1": "bit1_p29_wire_bytes"},
        ),
    )
    expected = {}
    for stage, path, keys in specifications:
        report = json.loads(path.read_text())
        for row in report["per_corpus"]:
            for state, key in keys.items():
                expected[(row["corpus"].lower(), stage, state)] = int(row[key])
    return expected


def checkpoint_point(curve: list[dict], checkpoint: int | None) -> dict | None:
    if checkpoint is not None and len(curve) < checkpoint:
        return None
    row = curve[-1] if checkpoint is None else curve[checkpoint - 1]
    wire = int(row["cumulative_wire_bytes"])
    return {
        "tu": int(row["tu"]),
        "raw_bytes": int(row["cumulative_raw_bytes"]),
        "wire_bytes": wire,
        "decimal_mb": wire / DECIMAL_MB,
        "seconds_1gbit": wire / ONE_GBIT_BYTES_PER_SECOND,
        "ratio": float(row["cumulative_ratio"]),
    }


def make_rows(
    run_root: Path,
    repository: Path,
    checkpoints: tuple[int, ...],
) -> list[dict]:
    expected = historical_endpoints(repository)
    output = []
    for corpus, _ in CORPORA:
        for schema in SCHEMAS:
            curve_path = run_root / schema["schema"] / f"{corpus}.curve.tsv"
            log_path = run_root / schema["schema"] / f"{corpus}.log"
            curve = load_curve(curve_path)
            if not log_path.is_file():
                raise FileNotFoundError(log_path)
            full = checkpoint_point(curve, None)
            assert full is not None
            key = (corpus, schema["stage"], schema["state"])
            if full["wire_bytes"] != expected[key]:
                raise ValueError(
                    f"{schema['schema']}/{corpus}: full wire {full['wire_bytes']} "
                    f"differs from retained {expected[key]}"
                )
            row = {
                "project": corpus,
                "schema": schema["schema"],
                "stage": schema["stage"],
                "receiver_state": schema["state"],
                "description": schema["description"],
                "tus": len(curve),
                "raw_bytes": full["raw_bytes"],
                "curve_sha256": sha256(curve_path),
                "log_sha256": sha256(log_path),
            }
            for checkpoint in checkpoints:
                point = checkpoint_point(curve, checkpoint)
                prefix = f"tu{checkpoint}"
                row[f"{prefix}_wire_bytes"] = "" if point is None else point["wire_bytes"]
                row[f"{prefix}_decimal_mb"] = "" if point is None else point["decimal_mb"]
                row[f"{prefix}_seconds_1gbit"] = "" if point is None else point["seconds_1gbit"]
            row["full_wire_bytes"] = full["wire_bytes"]
            row["full_decimal_mb"] = full["decimal_mb"]
            row["full_seconds_1gbit"] = full["seconds_1gbit"]
            row["full_ratio"] = full["ratio"]
            row["exact"] = True
            output.append(row)
    return output


def write_tsv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=tuple(rows[0]), delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def render_markdown(
    rows: list[dict], checkpoints: tuple[int, ...], run_root: Path
) -> str:
    lines = [
        "# Complete-codec per-project/per-schema TU transfer matrix",
        "",
        "Every data row is one **project × complete codec schema**. Each checkpoint cell is "
        "`cumulative decimal MB transferred / ideal seconds at 1 Gbit/s`. A dash means the "
        "project completed before that absolute TU. `full` is the complete build in the receiver "
        "state named by the schema.",
        "",
        "All rows reconstruct the complete `.ii` byte stream exactly. The full endpoint of every "
        "row is checked against its previously retained P25, P26, P27, P28, or P29 ledger; this "
        "table is not derived from archive sizes or structural-only estimates.",
        "",
        "The execution profile keeps original source files visible where the complete codec can "
        "reuse them. Quietbox2 supplied 210 rows. OpenCV and LevelDB retain absolute source paths "
        "from the capture host, so all 30 rows for those two projects were rerun on that host; "
        "using the copied `.ii` files alone changes their material decisions. P26 is checked "
        "against the same-binary P26 controls in the P27 ledger, which uses this same source-visible "
        "profile.",
        "",
        "## Schema legend",
        "",
        "| schema prefix | complete-codec change |",
        "|---|---|",
    ]
    for stage in ("p25", "p26", "p27", "p28", "p29"):
        schema = next(value for value in SCHEMAS if value["stage"] == stage)
        lines.append(f"| `{stage}` | {schema['description']} |")
    lines.extend(
        [
            "",
            "Receiver suffixes are `cold` (empty F generation cache), `bit0`, and `bit1` "
            "(the two exact complementary half-cold Region-key partitions). P26 is the smart "
            "zlib-member recovery lane; P27 adds canonical-MO factoring; P29 adds selective "
            "zstd-9/LDM compression only to the admitted embedded-object payload.",
            "",
            "## Matrix",
            "",
            "| project | schema | "
            + " | ".join(f"TU {value} MB / s" for value in checkpoints)
            + " | full MB / s | full ratio |",
            "|---|---|" + "---:|" * (len(checkpoints) + 2),
        ]
    )
    for row in rows:
        cells = []
        for checkpoint in checkpoints:
            value = row[f"tu{checkpoint}_decimal_mb"]
            if value == "":
                cells.append("—")
            else:
                cells.append(
                    f"{float(value):.6f} / {float(row[f'tu{checkpoint}_seconds_1gbit']):.6f}"
                )
        lines.append(
            f"| {row['project']} | `{row['schema']}` | "
            + " | ".join(cells)
            + f" | {float(row['full_decimal_mb']):.6f} / {float(row['full_seconds_1gbit']):.6f}"
            + f" | {float(row['full_ratio']):.3f}x |"
        )
    lines.extend(
        [
            "",
            "## Accounting",
            "",
            "```text",
            "decimal_MB = cumulative_complete_protocol_wire_bytes / 1,000,000",
            "seconds_at_1_Gbit/s = cumulative_complete_protocol_wire_bytes / 125,000,000",
            "```",
            "",
            "The machine TSV retains exact byte counts alongside the display values.",
            f"The 240 per-TU curves and 240 complete logs are retained under `{run_root.resolve()}`; "
            "the machine summary records a SHA-256 for each.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument("--checkpoints", nargs="+", type=int, default=DEFAULT_CHECKPOINTS)
    parser.add_argument("--tsv", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()
    checkpoints = tuple(args.checkpoints)
    rows = make_rows(args.run_root, args.repository, checkpoints)
    for path in (args.tsv, args.json, args.markdown):
        path.parent.mkdir(parents=True, exist_ok=True)
    write_tsv(args.tsv, rows)
    summary = {
        "experiment": "complete P25-P29 per-project/per-schema TU matrix",
        "execution_profile": {
            "name": "source-visible consistent",
            "quietbox2_rows": 210,
            "capture_host_opencv_leveldb_rows": 30,
            "reason": (
                "OpenCV and LevelDB .ii marker paths resolve only on the capture host; "
                "all schemas for those projects use that host"
            ),
        },
        "accounting": {
            "decimal_mb_divisor": DECIMAL_MB,
            "one_gbit_bytes_per_second": ONE_GBIT_BYTES_PER_SECOND,
        },
        "checkpoints": list(checkpoints),
        "retained_run_root": str(args.run_root.resolve()),
        "exact_rows": len(rows),
        "rows": rows,
    }
    args.json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    args.markdown.write_text(render_markdown(rows, checkpoints, args.run_root))
    print(f"wrote {len(rows)} exact project/schema rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
