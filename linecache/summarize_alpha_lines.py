#!/usr/bin/env python3
"""Build the deterministic issue-16 P4 alpha-Line cold ledger."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path


BYTE_EXACT_RE = re.compile(
    r"byte-exact=(?P<exact>\w+)\s+TUs=(?P<tus>\d+)\s+raw=(?P<raw_mib>[0-9.]+) MiB"
)
TOTAL_RE = re.compile(r"\bTOTAL=(?P<total>\d+)")
SPLIT_RE = re.compile(
    r"C-encode (?P<c>[0-9.]+) GB/s \| F-decode (?P<f>[0-9.]+) GB/s"
)
ALPHA_RE = re.compile(r"^alpha lines:\s+(?P<fields>.+)$", re.MULTILINE)
FIELD_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<value>[0-9]+)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-json", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    return parser.parse_args()


def parse_log(path: Path) -> dict[str, int | float | str | bool]:
    raw = path.read_bytes()
    text = raw.decode()
    byte_exact = BYTE_EXACT_RE.search(text)
    total = TOTAL_RE.search(text)
    split = SPLIT_RE.search(text)
    alpha = ALPHA_RE.search(text)
    if not byte_exact or not total or not split or not alpha:
        raise ValueError(f"incomplete alpha log: {path}")
    fields = {match["key"]: int(match["value"]) for match in FIELD_RE.finditer(alpha["fields"])}
    required = {
        "input_raw",
        "eligible_lines",
        "gap_raw",
        "ordinary_candidate_wire",
        "literal_keyword_candidate_wire",
        "parameterized_keyword_candidate_wire",
        "best_alpha_candidate_wire",
        "selected_wire",
        "selector_wire",
        "selected_tus",
        "ordinary_tus",
        "literal_keyword_tus",
        "parameterized_keyword_tus",
    }
    missing = required - fields.keys()
    if missing:
        raise ValueError(f"missing alpha fields in {path}: {sorted(missing)}")
    return {
        "corpus": path.stem,
        "exact": byte_exact["exact"] == "OK",
        "tus": int(byte_exact["tus"]),
        "wire_bytes": int(total["total"]),
        "c_gbps": float(split["c"]),
        "f_gbps": float(split["f"]),
        "log_sha256": hashlib.sha256(raw).hexdigest(),
        **fields,
    }


def main() -> None:
    args = parse_args()
    baseline = json.loads(args.baseline_json.read_text())
    baseline_rows = {row["corpus"]: row for row in baseline["per_corpus"]}
    rows = [parse_log(path) for path in sorted(args.logs.glob("*.log"))]
    if {row["corpus"] for row in rows} != set(baseline_rows):
        raise ValueError("alpha logs do not match baseline corpus set")

    for row in rows:
        control = baseline_rows[str(row["corpus"])]
        if row["tus"] != control["tus"]:
            raise ValueError(f"TU count differs for {row['corpus']}")
        row["raw_bytes"] = control["raw_bytes"]
        row["p26_wire_bytes"] = control["cold_p26_wire_bytes"]
        row["delta_vs_p26"] = int(row["wire_bytes"]) - int(row["p26_wire_bytes"])
        row["saving_vs_independent"] = int(row["ordinary_candidate_wire"]) - int(
            row["selected_wire"]
        )
        row["independent_ordinary_complete_wire"] = int(row["wire_bytes"]) + int(
            row["saving_vs_independent"]
        )
        row["p26_ratio"] = int(row["raw_bytes"]) / int(row["p26_wire_bytes"])
        row["alpha_ratio"] = int(row["raw_bytes"]) / int(row["wire_bytes"])

    integer_totals = [
        "raw_bytes",
        "tus",
        "p26_wire_bytes",
        "wire_bytes",
        "delta_vs_p26",
        "ordinary_candidate_wire",
        "literal_keyword_candidate_wire",
        "parameterized_keyword_candidate_wire",
        "best_alpha_candidate_wire",
        "selected_wire",
        "selector_wire",
        "saving_vs_independent",
        "independent_ordinary_complete_wire",
        "selected_tus",
        "ordinary_tus",
        "literal_keyword_tus",
        "parameterized_keyword_tus",
        "input_raw",
        "eligible_lines",
        "gap_raw",
    ]
    aggregate = {key: sum(int(row[key]) for row in rows) for key in integer_totals}
    aggregate["exact_corpora"] = sum(bool(row["exact"]) for row in rows)
    aggregate["corpora"] = len(rows)
    aggregate["p26_ratio"] = aggregate["raw_bytes"] / aggregate["p26_wire_bytes"]
    aggregate["alpha_ratio"] = aggregate["raw_bytes"] / aggregate["wire_bytes"]
    aggregate["cold_400_limit_bytes"] = aggregate["raw_bytes"] / 400
    aggregate["p26_gap_to_cold_400"] = aggregate["p26_wire_bytes"] - aggregate[
        "cold_400_limit_bytes"
    ]
    aggregate["alpha_gap_to_cold_400"] = aggregate["wire_bytes"] - aggregate[
        "cold_400_limit_bytes"
    ]
    aggregate["five_mb_stop_rule_passed"] = aggregate["saving_vs_independent"] >= 5_000_000

    document = {
        "experiment": "P4 alpha-normalized whole-Line residual selector",
        "scope": "complete cold chronological 16-corpus execution",
        "aggregate": aggregate,
        "per_corpus": rows,
    }
    args.json.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")

    columns = [
        "corpus",
        "raw_bytes",
        "tus",
        "exact",
        "p26_wire_bytes",
        "wire_bytes",
        "delta_vs_p26",
        "p26_ratio",
        "alpha_ratio",
        "ordinary_candidate_wire",
        "selected_wire",
        "saving_vs_independent",
        "selected_tus",
        "ordinary_tus",
        "c_gbps",
        "f_gbps",
        "log_sha256",
    ]
    with args.tsv.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=columns,
            delimiter="\t",
            extrasaction="ignore",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
