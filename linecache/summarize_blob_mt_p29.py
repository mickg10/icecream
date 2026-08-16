#!/usr/bin/env python3
"""Summarize P29's selective multithreaded embedded-blob encoding."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from summarize_complete_codec50 import CATEGORIES, CORPORA, aggregate, normalize_timed_log
from summarize_half_cold_codec import parse_key_map


STATES = ("cold", "bit0", "bit1")
EXPECTED_BITS = {"cold": -1, "bit0": 0, "bit1": 1}
UNCHANGED_CATEGORIES = (
    "root",
    "region_def",
    "block_def",
    "path_def",
    "missing",
    "framing",
)
S1 = re.compile(
    r"S1 LZ: [0-9.]+s min_match=(\d+) max_chain=(\d+) "
    r"tokens=(\d+) blocks=(\d+)"
)
MATERIAL_LEVELS = re.compile(
    r"material zstd levels: control=(\d+) literal=(\d+) "
    r"array=(\d+) blob=(\d+)"
)
BLOB_POLICY = re.compile(
    r"compressed blobs:.*?threads=(\d+) zstd_workers=(\d+) "
    r"zstd_job_mib=(\d+) zstd_overlap_log=(\d+) mode=([^ ]+)"
)


def log_path(directory: Path, index: int, corpus: str) -> Path:
    named = directory / f"{corpus}.log"
    return named if named.exists() else directory / f"corpus{index}.log"


def parse_p29(path: Path, expected_bit: int) -> dict[str, object]:
    row = parse_key_map(path, expected_bit)
    text = normalize_timed_log(path.read_text())
    s1 = S1.search(text)
    levels = MATERIAL_LEVELS.search(text)
    policy = BLOB_POLICY.search(text)
    if s1 is None:
        raise ValueError(f"{path}: missing S1 ledger")
    if levels is None:
        raise ValueError(f"{path}: missing material-level ledger")
    if policy is None:
        raise ValueError(f"{path}: missing blob-worker ledger")
    minimum, chain, tokens, blocks = map(int, s1.groups())
    if (minimum, chain) != (3, 1024):
        raise ValueError(f"{path}: expected S1 3/1024, got {minimum}/{chain}")
    material_levels = tuple(map(int, levels.groups()))
    if material_levels != (3, 3, 3, 9):
        raise ValueError(f"{path}: unexpected material levels {material_levels}")
    blob_threads, workers, job_mib, overlap_log = map(int, policy.groups()[:4])
    mode = policy.group(5)
    if (blob_threads, workers, job_mib, overlap_log, mode) != (
        8,
        4,
        5,
        3,
        "lazy-reply",
    ):
        raise ValueError(f"{path}: unexpected blob policy {policy.groups()}")
    row.update(
        {
            "s1_tokens": tokens,
            "s1_blocks": blocks,
            "material_control_zstd_level": material_levels[0],
            "material_literal_zstd_level": material_levels[1],
            "material_array_zstd_level": material_levels[2],
            "material_blob_zstd_level": material_levels[3],
            "blob_threads": blob_threads,
            "blob_zstd_workers": workers,
            "blob_zstd_job_mib": job_mib,
            "blob_zstd_overlap_log": overlap_log,
            "blob_mode": mode,
        }
    )
    return row


def target(state: str, row: dict[str, object]) -> dict[str, object]:
    divisor = 400 if state == "cold" else 200
    raw = int(row["raw_bytes"])
    wire = int(row["total_wire_bytes"])
    allowance = raw / divisor
    return {
        "ratio": divisor,
        "allowance_bytes": allowance,
        "gap_bytes": wire - allowance,
        "passed": wire <= allowance,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--p28-summary", type=Path, required=True)
    for state in STATES:
        parser.add_argument(f"--{state}-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    p28 = json.loads(args.p28_summary.read_text())
    p28_corpora = {item["corpus"]: item for item in p28["per_corpus"]}
    if tuple(p28_corpora) != CORPORA:
        raise ValueError("P28 corpus order differs from the fixed suite")

    per_corpus: list[dict[str, object]] = [{"corpus": corpus} for corpus in CORPORA]
    states: dict[str, object] = {}
    for state in STATES:
        bit = EXPECTED_BITS[state]
        directory = getattr(args, f"{state}_dir")
        candidates: list[dict[str, object]] = []
        p28_total = 0
        for index, (corpus, item) in enumerate(zip(CORPORA, per_corpus), 1):
            baseline = p28_corpora[corpus]
            candidate = parse_p29(log_path(directory, index, corpus), bit)
            baseline_wire = int(baseline[f"{state}_p28_wire_bytes"])
            candidate_wire = int(candidate["total_wire_bytes"])
            saving = baseline_wire - candidate_wire
            if int(candidate["tus"]) != int(baseline["tus"]):
                raise ValueError(f"{state}/{corpus}: TU count differs from P28")
            if int(candidate["raw_bytes"]) != int(baseline["raw_bytes"]):
                raise ValueError(f"{state}/{corpus}: raw bytes differ from P28")
            if int(candidate["s1_tokens"]) != int(baseline[f"{state}_s1_tokens"]):
                raise ValueError(f"{state}/{corpus}: S1 token count differs from P28")
            if int(candidate["s1_blocks"]) != int(baseline[f"{state}_s1_blocks"]):
                raise ValueError(f"{state}/{corpus}: S1 block count differs from P28")
            if corpus == "godot":
                if saving <= 0 or int(candidate["blob_count"]) == 0:
                    raise ValueError(f"{state}/{corpus}: selected blob lane did not improve")
            elif saving or int(candidate["blob_count"]):
                raise ValueError(f"{state}/{corpus}: unexpected non-Godot change")
            raw = int(candidate["raw_bytes"])
            item.update(
                {
                    "tus": candidate["tus"],
                    "raw_bytes": raw,
                    f"{state}_p28_wire_bytes": baseline_wire,
                    f"{state}_p29_wire_bytes": candidate_wire,
                    f"{state}_saving_bytes": saving,
                    f"{state}_p29_ratio": raw / candidate_wire,
                    f"{state}_p29_pipeline_gbps": candidate["pipeline_gbps"],
                    f"{state}_blob_count": candidate["blob_count"],
                    f"{state}_blob_inflated_bytes": candidate["blob_inflated_bytes"],
                    f"{state}_blob_wire_bytes": candidate["blob_wire_bytes"],
                    f"{state}_log_sha256": candidate["log_sha256"],
                }
            )
            for category in CATEGORIES[:-1]:
                item[f"{state}_{category}_wire_bytes"] = candidate[
                    f"{category}_wire_bytes"
                ]
            candidates.append(candidate)
            p28_total += baseline_wire

        p29_aggregate = aggregate(candidates)
        p28_aggregate = p28["states"][state]["p28"]
        if p28_total != int(p28_aggregate["total_wire_bytes"]):
            raise ValueError(f"{state}: P28 per-corpus total does not close")
        for category in UNCHANGED_CATEGORIES:
            field = f"{category}_wire_bytes"
            if int(p29_aggregate[field]) != int(p28_aggregate[field]):
                raise ValueError(f"{state}: unrelated {category} leg changed")
        line_saving = int(p28_aggregate["line_def_wire_bytes"]) - int(
            p29_aggregate["line_def_wire_bytes"]
        )
        total_saving = p28_total - int(p29_aggregate["total_wire_bytes"])
        if total_saving != line_saving or total_saving <= 0:
            raise ValueError(f"{state}: selected material saving does not close")
        states[state] = {
            "p28": p28_aggregate,
            "p29": p29_aggregate,
            "saving_bytes": total_saving,
            "changed_corpora": [
                item["corpus"]
                for item in per_corpus
                if int(item[f"{state}_saving_bytes"]) != 0
            ],
            "blob_count": sum(int(row["blob_count"]) for row in candidates),
            "blob_inflated_bytes": sum(
                int(row["blob_inflated_bytes"]) for row in candidates
            ),
            "blob_wire_bytes": sum(int(row["blob_wire_bytes"]) for row in candidates),
            "target": target(state, p29_aggregate),
        }

    report = {
        "experiment": "P29 selective zstd-9/LDM embedded-blob lane with bounded workers",
        "scope": (
            "complete direct-ordinal P28 wire in cold and complementary cache states; "
            "only embedded-blob material uses zstd-9 with four workers, 5 MiB jobs, "
            "and overlap-log 3; every reconstructed .ii is exact"
        ),
        "policy": {
            "control_zstd_level": 3,
            "literal_zstd_level": 3,
            "array_zstd_level": 3,
            "blob_zstd_level": 9,
            "blob_threads": 8,
            "blob_zstd_workers": 4,
            "blob_zstd_job_mib": 5,
            "blob_zstd_overlap_log": 3,
            "blob_mode": "lazy-reply",
        },
        "complete_runs": len(STATES) * len(CORPORA),
        "exact_runs": len(STATES) * len(CORPORA),
        "all_per_corpus_states_non_regressing": all(
            int(item[f"{state}_saving_bytes"]) >= 0
            for item in per_corpus
            for state in STATES
        ),
        "states": states,
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
    print(
        json.dumps(
            {key: value for key, value in report.items() if key != "per_corpus"},
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
