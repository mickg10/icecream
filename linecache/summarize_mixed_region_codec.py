#!/usr/bin/env python3
"""Summarize exact P21 versus P24 full-file reconstruction ledgers."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from summarize_complete_codec50 import (
    CATEGORIES,
    CORPORA,
    aggregate,
    normalize_timed_log,
    parse_log,
)


MIXED_PATTERN = re.compile(
    r"mixed components: control=(\d+) literal=(\d+) array_control=(\d+) "
    r"array_values=(\d+) source_control=(\d+) source_files=(\d+) selector=(\d+) "
    r"raw_literal=(\d+) raw_array_values=(\d+) raw_source_reused=(\d+) "
    r"source_package_raw=(\d+) source_package_files=(\d+) public_lines=(\d+) "
    r"ops=\[literal=(\d+) publish=(\d+) ref=(\d+) array=(\d+) marker=(\d+) "
    r"source=(\d+) patch=(\d+)\]"
)

MIXED_FIELDS = (
    "control_wire_bytes",
    "literal_wire_bytes",
    "array_control_wire_bytes",
    "array_values_wire_bytes",
    "source_control_wire_bytes",
    "source_files_wire_bytes",
    "selector_wire_bytes",
    "literal_raw_bytes",
    "array_values_raw_bytes",
    "source_reused_raw_bytes",
    "source_package_raw_bytes",
    "source_package_files",
    "public_lines",
    "literal_ops",
    "publish_ops",
    "public_ref_ops",
    "array_ops",
    "marker_ops",
    "source_copy_ops",
    "source_patch_ops",
    "blob_count",
    "blob_deflated_bytes",
    "blob_inflated_bytes",
    "blob_wire_bytes",
    "blob_patch_raw_bytes",
    "blob_patch_wire_bytes",
    "blob_canonical_exact",
    "blob_corrected",
    "blob_replaced",
    "blob_transform_candidate_wire_bytes",
    "blob_ordinary_candidate_wire_bytes",
    "blob_transform_tus",
    "blob_ordinary_tus",
    "blob_fallbacks",
    "blob_fallback_request_wire_bytes",
    "blob_fallback_reply_wire_bytes",
    "blob_threads",
)

BLOB_PATTERN = re.compile(
    r"compressed blobs: count=(\d+) deflated=(\d+) inflated=(\d+) wire=(\d+) "
    r"patch_raw=(\d+) patch_wire=(\d+) canonical_exact=(\d+) corrected=(\d+) "
    r"replaced=(\d+) transform_candidate_wire=(\d+) ordinary_candidate_wire=(\d+) "
    r"transform_tus=(\d+) ordinary_tus=(\d+) "
    r"fallbacks=(\d+) fallback_request_wire=(\d+) fallback_reply_wire=(\d+) "
    r"threads=(\d+)"
)

BLOB_FIELDS = MIXED_FIELDS[-17:]
BASE_MIXED_FIELDS = MIXED_FIELDS[:-17]


def parse_mixed(path: Path) -> dict[str, object]:
    row = parse_log(path)
    text = normalize_timed_log(path.read_text())
    found = MIXED_PATTERN.search(text)
    if found is None:
        raise ValueError(f"{path}: missing mixed-component ledger")
    row.update(dict(zip(BASE_MIXED_FIELDS, map(int, found.groups()))))
    blob = BLOB_PATTERN.search(text)
    row.update(
        dict(zip(BLOB_FIELDS, map(int, blob.groups())))
        if blob is not None
        else {field: 0 for field in BLOB_FIELDS}
    )
    if (
        int(row["control_wire_bytes"]) != int(row["region_def_wire_bytes"])
        or sum(
            int(row[name])
            for name in (
                "literal_wire_bytes",
                "array_control_wire_bytes",
                "array_values_wire_bytes",
                "source_control_wire_bytes",
                "source_files_wire_bytes",
                "selector_wire_bytes",
                "blob_wire_bytes",
                "blob_patch_wire_bytes",
            )
        )
        != int(row["line_def_wire_bytes"])
    ):
        raise ValueError(f"{path}: mixed subcomponents do not close their categories")
    return row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--p21-dir", type=Path, required=True)
    parser.add_argument("--p24-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    variants: dict[str, list[dict[str, object]]] = {"p21": [], "p24": []}
    per_corpus: list[dict[str, object]] = []
    for corpus in CORPORA:
        p21 = parse_log(args.p21_dir / f"{corpus}.log")
        p24 = parse_mixed(args.p24_dir / f"{corpus}.log")
        for field in (
            "tus",
            "raw_bytes",
            "regions",
            "region_occurrences",
            "distinct_lines",
        ):
            if p21[field] != p24[field]:
                raise ValueError(f"{corpus}: P21/P24 {field} differs")
        for category in ("root", "block_def", "missing", "framing"):
            field = f"{category}_wire_bytes"
            if p21[field] != p24[field]:
                raise ValueError(f"{corpus}: unrelated {category} leg changed")
        variants["p21"].append(p21)
        variants["p24"].append(p24)
        raw = int(p21["raw_bytes"])
        p21_wire = int(p21["total_wire_bytes"])
        p24_wire = int(p24["total_wire_bytes"])
        item: dict[str, object] = {
            "corpus": corpus,
            "tus": p21["tus"],
            "raw_bytes": raw,
            "exact": True,
            "p21_wire_bytes": p21_wire,
            "p24_wire_bytes": p24_wire,
            "saving_bytes": p21_wire - p24_wire,
            "p21_ratio": raw / p21_wire,
            "p24_ratio": raw / p24_wire,
            "p24_pipeline_gbps": p24["pipeline_gbps"],
            "p21_log_sha256": p21["log_sha256"],
            "p24_log_sha256": p24["log_sha256"],
        }
        for category in CATEGORIES[:-1]:
            item[f"p21_{category}_wire_bytes"] = p21[f"{category}_wire_bytes"]
            item[f"p24_{category}_wire_bytes"] = p24[f"{category}_wire_bytes"]
        for field in MIXED_FIELDS:
            item[f"p24_{field}"] = p24[field]
        per_corpus.append(item)

    aggregates = {name: aggregate(rows) for name, rows in variants.items()}
    p24_mixed = {
        field: sum(int(row[field]) for row in variants["p24"])
        for field in MIXED_FIELDS
    }
    report = {
        "experiment": "P24 exact mixed-Region materializer",
        "scope": (
            "one empty-receiver dense-ID conversation; exact full-.ii reconstruction "
            "with mixed Region programs, raw completed Regions, public Line views, "
            "BYTE_ARRAY, compiler-environment source copies, Roots/Blocks, paths, "
            "missing lists, and framing"
        ),
        "full_file_reconstruction_complete": True,
        "total_protocol_acceptance_complete": False,
        "remaining_acceptance_blocks": [
            "generation-key and dense-map accounting for nonempty/half-cold receiver state",
            "actual C/F socket framing and ordered-lane lifecycle",
            "complete complementary half-cold object-cache execution",
            "multi-F assignment, bounded eviction, and reorder/change/revert replay",
        ],
        "aggregates": aggregates,
        "p24_mixed_aggregates": p24_mixed,
        "saving_vs_p21_bytes": int(aggregates["p21"]["total_wire_bytes"])
        - int(aggregates["p24"]["total_wire_bytes"]),
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
