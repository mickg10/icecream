#!/usr/bin/env python3
"""Summarize exact full-file CODEC-50 reconstruction logs for issue #16."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path


CORPORA = (
    "llvm",
    "rocksdb",
    "duckdb",
    "abseil",
    "opencv",
    "godot",
    "fmt",
    "spdlog",
    "catch2",
    "nlohmann-json",
    "range-v3",
    "eigen",
    "re2",
    "leveldb",
    "simdjson",
    "cereal",
)
CATEGORIES = (
    "root",
    "line_def",
    "region_def",
    "block_def",
    "path_def",
    "missing",
    "framing",
    "total",
)

TIME_RECORD = re.compile(r"peak RSS=[0-9.]+ MiB total=[0-9.]+s\n")


def normalize_timed_log(text: str) -> str:
    """Remove a timing record even when stderr inserted it inside stdout."""

    return TIME_RECORD.sub("", text)


def match(pattern: str, text: str, source: Path) -> re.Match[str]:
    found = re.search(pattern, text)
    if found is None:
        raise ValueError(f"{source}: missing log record: {pattern}")
    return found


def parse_log(path: Path) -> dict[str, object]:
    raw_log = path.read_bytes()
    raw_text = raw_log.decode()
    text = normalize_timed_log(raw_text)
    load = match(
        r"loaded\+interned ([0-9.]+)s TUs=(\d+) raw=(\d+) "
        r"regions=(\d+) region_occ=(\d+) distinct_lines=(\d+)",
        text,
        path,
    )
    result = match(
        r"byte-exact=(\w+)\s+TUs=(\d+) raw=([0-9.]+) MiB "
        r"regions=(\d+) distinct_lines=(\d+) paths=(\d+) blocks=(\d+)",
        text,
        path,
    )
    wire = match(
        r"wire by category .*?root=(\d+) line_def=(\d+) region_def=(\d+) "
        r"block_def=(\d+) path_def=(\d+) missing=(\d+) framing=(\d+)\s+"
        r"TOTAL=(\d+)",
        text,
        path,
    )
    speeds = match(
        r"C-encode ([0-9.]+) GB/s \| F-decode ([0-9.]+) GB/s "
        r"=> pipelined min = ([0-9.]+) GB/s",
        text,
        path,
    )
    peak = match(r"peak RSS=([0-9.]+) MiB", raw_text, path)
    values = dict(zip(CATEGORIES, map(int, wire.groups())))
    if sum(values[name] for name in CATEGORIES[:-1]) != values["total"]:
        raise ValueError(f"{path}: category sum differs from total")
    if result.group(1) != "OK" or result.group(2) != load.group(2):
        raise ValueError(f"{path}: incomplete or inexact run")
    row: dict[str, object] = {
        "tus": int(load.group(2)),
        "raw_bytes": int(load.group(3)),
        "regions": int(load.group(4)),
        "region_occurrences": int(load.group(5)),
        "distinct_lines": int(load.group(6)),
        "paths": int(result.group(6)),
        "blocks": int(result.group(7)),
        "exact": True,
        "encode_gbps": float(speeds.group(1)),
        "decode_gbps": float(speeds.group(2)),
        "pipeline_gbps": float(speeds.group(3)),
        "peak_rss_mib": float(peak.group(1)),
        "log_sha256": hashlib.sha256(raw_log).hexdigest(),
        **{f"{name}_wire_bytes": value for name, value in values.items()},
    }
    root = re.search(
        r"ROOT_SLICE stats: copies=(\d+) copied_regions=(\d+) "
        r"indexed_windows=(\d+) index_entries=(\d+) receiver_root_bytes=(\d+)",
        text,
    )
    if root:
        row.update(
            {
                "root_slice_copies": int(root.group(1)),
                "root_slice_copied_regions": int(root.group(2)),
                "root_slice_indexed_windows": int(root.group(3)),
                "root_slice_index_entries": int(root.group(4)),
                "root_slice_receiver_bytes": int(root.group(5)),
            }
        )
    return row


def aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
    raw = sum(int(row["raw_bytes"]) for row in rows)
    result: dict[str, object] = {
        "corpora": len(rows),
        "tus": sum(int(row["tus"]) for row in rows),
        "raw_bytes": raw,
        "exact_corpora": sum(bool(row["exact"]) for row in rows),
        "minimum_pipeline_gbps": min(float(row["pipeline_gbps"]) for row in rows),
        "maximum_peak_rss_mib": max(float(row["peak_rss_mib"]) for row in rows),
    }
    for category in CATEGORIES:
        result[f"{category}_wire_bytes"] = sum(
            int(row[f"{category}_wire_bytes"]) for row in rows
        )
    total = int(result["total_wire_bytes"])
    result["weighted_ratio"] = raw / total
    result["equal_corpus_ratio"] = len(rows) / sum(
        int(row["total_wire_bytes"]) / int(row["raw_bytes"]) for row in rows
    )
    result["cold_400_allowance_bytes"] = raw / 400
    result["gap_to_cold_400_bytes"] = total - raw / 400
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-dir", type=Path, required=True)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    per_corpus = []
    baseline_rows = []
    candidate_rows = []
    for corpus in CORPORA:
        baseline = parse_log(args.baseline_dir / f"{corpus}.log")
        candidate = parse_log(args.candidate_dir / f"{corpus}.log")
        for field in ("tus", "raw_bytes", "regions", "region_occurrences", "distinct_lines"):
            if baseline[field] != candidate[field]:
                raise ValueError(f"{corpus}: baseline/candidate {field} differs")
        for category in ("line_def", "region_def", "path_def"):
            field = f"{category}_wire_bytes"
            if baseline[field] != candidate[field]:
                raise ValueError(f"{corpus}: unrelated {category} leg changed")
        baseline_rows.append(baseline)
        candidate_rows.append(candidate)
        raw = int(baseline["raw_bytes"])
        base_wire = int(baseline["total_wire_bytes"])
        candidate_wire = int(candidate["total_wire_bytes"])
        per_corpus.append(
            {
                "corpus": corpus,
                "tus": baseline["tus"],
                "raw_bytes": raw,
                "baseline_wire_bytes": base_wire,
                "candidate_wire_bytes": candidate_wire,
                "wire_delta_bytes": candidate_wire - base_wire,
                "baseline_ratio": raw / base_wire,
                "candidate_ratio": raw / candidate_wire,
                "candidate_wins": candidate_wire < base_wire,
                "baseline_root_plus_block_wire_bytes": int(baseline["root_wire_bytes"])
                + int(baseline["block_def_wire_bytes"]),
                "candidate_root_plus_block_wire_bytes": int(candidate["root_wire_bytes"])
                + int(candidate["block_def_wire_bytes"]),
                "missing_delta_bytes": int(candidate["missing_wire_bytes"])
                - int(baseline["missing_wire_bytes"]),
                "framing_delta_bytes": int(candidate["framing_wire_bytes"])
                - int(baseline["framing_wire_bytes"]),
                "root_slice_copies": candidate.get("root_slice_copies", 0),
                "root_slice_copied_regions": candidate.get(
                    "root_slice_copied_regions", 0
                ),
                "exact": bool(baseline["exact"] and candidate["exact"]),
            }
        )

    baseline = aggregate(baseline_rows)
    candidate = aggregate(candidate_rows)
    report = {
        "experiment": "exact full-file CODEC-50 S1 versus P22 ROOT_SLICE",
        "scope": (
            "one empty-receiver dense-ID conversation; exact full-.ii reconstruction "
            "including Lines, Region composition, Roots/Blocks, paths, missing lists, "
            "and framing"
        ),
        "full_file_reconstruction_complete": True,
        "total_protocol_acceptance_complete": False,
        "remaining_acceptance_blocks": [
            "generation-key and dense-map accounting for nonempty/half-cold receiver state",
            "actual C/F socket framing and dispatch",
            "complete complementary half-cold object-cache execution",
        ],
        "baseline": baseline,
        "candidate": candidate,
        "candidate_wire_delta_bytes": int(candidate["total_wire_bytes"])
        - int(baseline["total_wire_bytes"]),
        "candidate_ratio_change_fraction": float(candidate["weighted_ratio"])
        / float(baseline["weighted_ratio"])
        - 1,
        "candidate_win_corpora": sum(row["candidate_wins"] for row in per_corpus),
        "per_corpus": per_corpus,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    with args.tsv.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(per_corpus[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(per_corpus)
    print(json.dumps({key: value for key, value in report.items() if key != "per_corpus"}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
