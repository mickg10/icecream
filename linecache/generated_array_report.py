#!/usr/bin/env python3
"""Validate and publish the balanced generated-array Line-program result."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any, Iterable


def load_json(path: str) -> dict[str, Any]:
    value = json.loads(Path(path).read_text())
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def digest(path: str) -> str:
    value = hashlib.sha256()
    with open(path, "rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def aggregate(rows: list[dict[str, Any]]) -> dict[str, Any]:
    require(bool(rows), "cannot aggregate an empty row set")
    raw = sum(int(row["raw_bytes"]) for row in rows)
    line = sum(int(row["line_bytes"]) for row in rows)
    wire = sum(int(row["wire_bytes"]) for row in rows)
    return {
        "corpora": len(rows),
        "raw_bytes": raw,
        "line_bytes": line,
        "wire_bytes": wire,
        "weighted_ratio": raw / wire,
        "equal_corpus_ratio": len(rows)
        / sum(int(row["wire_bytes"]) / int(row["raw_bytes"]) for row in rows),
        "exact": all(bool(row["exact"]) for row in rows),
    }


def markdown_table(headers: Iterable[str], rows: Iterable[Iterable[Any]]) -> str:
    headers = tuple(headers)
    output = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join("---" for _ in headers) + "|",
    ]
    output.extend("| " + " | ".join(map(str, row)) + " |" for row in rows)
    return "\n".join(output)


def build_summary(codec: dict[str, Any], structural: dict[str, Any]) -> dict[str, Any]:
    rows = codec.get("rows")
    selected = structural.get("selected")
    require(isinstance(rows, list), "codec report has no row array")
    require(isinstance(selected, list), "structural report has no selected row array")
    require(all(bool(row.get("exact")) for row in rows), "a codec row is not exact")
    require(all(bool(row.get("exact")) for row in selected), "a structural row is not exact")

    modes: dict[tuple[int, str], list[dict[str, Any]]] = {}
    for row in rows:
        modes.setdefault((int(row["level"]), str(row["mode"])), []).append(row)
    aggregates = {
        f"z{level}-{mode}": aggregate(group)
        for (level, mode), group in sorted(modes.items())
    }

    candidate_rows = modes.get((3, "stream-generated-array"), [])
    baseline_rows = modes.get((3, "stream-split-front"), [])
    require(len(candidate_rows) == 16, "z3 candidate does not contain 16 corpora")
    require(len(baseline_rows) == 16, "z3 baseline does not contain 16 corpora")
    candidate_by_corpus = {str(row["corpus"]): row for row in candidate_rows}
    baseline_by_corpus = {str(row["corpus"]): row for row in baseline_rows}
    structural_by_corpus = {str(row["corpus"]): row for row in selected}
    require(len(structural_by_corpus) == 16, "structural ledger does not contain 16 corpora")
    require(
        set(candidate_by_corpus) == set(baseline_by_corpus) == set(structural_by_corpus),
        "codec and structural corpus sets differ",
    )

    per_corpus = []
    for corpus in sorted(candidate_by_corpus):
        candidate = candidate_by_corpus[corpus]
        baseline = baseline_by_corpus[corpus]
        structure = structural_by_corpus[corpus]
        raw = int(candidate["raw_bytes"])
        require(raw == int(baseline["raw_bytes"]), f"{corpus}: codec raw totals differ")
        require(raw == int(structure["raw_bytes"]), f"{corpus}: structural raw total differs")
        baseline_wire = int(baseline["wire_bytes"])
        candidate_wire = int(candidate["wire_bytes"])
        structural_wire = int(structure["charged_wire_bytes"])
        cold_wire = structural_wire + candidate_wire
        half_cold_wire = structural_wire + candidate_wire / 2
        per_corpus.append(
            {
                "corpus": corpus,
                "raw_bytes": raw,
                "array_lines": int(candidate["array_lines"]),
                "array_text_bytes": int(candidate["array_text_bytes"]),
                "array_values": int(candidate["array_values"]),
                "baseline_line_wire_bytes": baseline_wire,
                "generated_array_line_wire_bytes": candidate_wire,
                "line_saving_bytes": baseline_wire - candidate_wire,
                "line_change_fraction": candidate_wire / baseline_wire - 1,
                "structural_wire_bytes": structural_wire,
                "cold_projection_wire_bytes": cold_wire,
                "cold_projection_ratio": raw / cold_wire,
                "half_cold_projection_wire_bytes": half_cold_wire,
                "half_cold_projection_ratio": raw / half_cold_wire,
                "exact": True,
            }
        )

    total_raw = sum(row["raw_bytes"] for row in per_corpus)
    total_structural = sum(row["structural_wire_bytes"] for row in per_corpus)
    total_line = sum(row["generated_array_line_wire_bytes"] for row in per_corpus)
    total_cold = total_structural + total_line
    total_half = total_structural + total_line / 2
    integrated = {
        "raw_bytes": total_raw,
        "structural_wire_bytes": total_structural,
        "line_wire_bytes": total_line,
        "cold_projection_wire_bytes": total_cold,
        "cold_weighted_ratio": total_raw / total_cold,
        "cold_equal_corpus_ratio": len(per_corpus)
        / sum(row["cold_projection_wire_bytes"] / row["raw_bytes"] for row in per_corpus),
        "cold_400_pass_corpora": sum(
            row["cold_projection_ratio"] >= 400 for row in per_corpus
        ),
        "half_cold_projection_wire_bytes": total_half,
        "half_cold_weighted_ratio": total_raw / total_half,
        "half_cold_equal_corpus_ratio": len(per_corpus)
        / sum(
            row["half_cold_projection_wire_bytes"] / row["raw_bytes"]
            for row in per_corpus
        ),
        "half_cold_200_pass_corpora": sum(
            row["half_cold_projection_ratio"] >= 200 for row in per_corpus
        ),
        "half_cold_is_projection": True,
    }

    components = {
        name: sum(int(row["component_wire"].get(name, 0)) for row in candidate_rows)
        for name in (
            "rest_lcp",
            "rest_length",
            "rest_suffix",
            "array_control",
            "array_values",
        )
    }
    components["selectors"] = sum(int(row["selector_wire_bytes"]) for row in candidate_rows)
    require(sum(components.values()) == total_line, "candidate component ledger does not sum")

    baseline = aggregates["z3-stream-split-front"]
    candidate = aggregates["z3-stream-generated-array"]
    return {
        "experiment": "strict generated decimal-byte array Line program",
        "format": 1,
        "corpora": len(per_corpus),
        "levels": codec.get("levels"),
        "max_tus": codec.get("max_tus"),
        "traces": codec.get("traces"),
        "aggregates": aggregates,
        "z3_line_saving_bytes": baseline["wire_bytes"] - candidate["wire_bytes"],
        "z3_line_saving_fraction": 1 - candidate["wire_bytes"] / baseline["wire_bytes"],
        "z3_candidate_components": components,
        "integrated_projection": integrated,
        "per_corpus": per_corpus,
        "exact": True,
    }


def render_report(
    summary: dict[str, Any], codec_path: str, structural_path: str
) -> str:
    aggregates = summary["aggregates"]
    integrated = summary["integrated_projection"]
    per_corpus = summary["per_corpus"]
    components = summary["z3_candidate_components"]
    aggregate_rows = []
    for level in (1, 3):
        for mode in (
            "independent-best",
            "stream-split-front",
            "stream-generated-array",
        ):
            value = aggregates[f"z{level}-{mode}"]
            aggregate_rows.append(
                (
                    f"zstd-{level}",
                    mode,
                    f"{value['wire_bytes']:,}",
                    f"{value['weighted_ratio']:.2f}x",
                    f"{value['equal_corpus_ratio']:.2f}x",
                    "yes" if value["exact"] else "no",
                )
            )

    corpus_rows = []
    for row in sorted(per_corpus, key=lambda value: value["cold_projection_ratio"]):
        corpus_rows.append(
            (
                row["corpus"],
                f"{row['array_text_bytes']:,}",
                f"{row['baseline_line_wire_bytes']:,}",
                f"{row['generated_array_line_wire_bytes']:,}",
                f"{row['line_saving_bytes']:+,}",
                f"{row['cold_projection_ratio']:.2f}x",
                f"{row['half_cold_projection_ratio']:.2f}x",
            )
        )

    component_rows = [
        (name, f"{wire:,}", f"{100 * wire / integrated['line_wire_bytes']:.2f}%")
        for name, wire in components.items()
    ]
    trace_arguments = (" " + "\\" + "\n").join(
        f"  --trace {trace['name']}={trace['path']}"
        for trace in summary["traces"]
    )

    return f"""# P20: exact generated-byte-array Line program across 16 corpora

## Verdict

Keep one narrow `BYTE_ARRAY` operation in the Line-definition candidate set. Across all 16 balanced
corpora at zstd-3, it reduces the stateful split-front Line plane from
`{aggregates['z3-stream-split-front']['wire_bytes']:,}` to
`{aggregates['z3-stream-generated-array']['wire_bytes']:,}` bytes: a
`{summary['z3_line_saving_bytes']:,}`-byte or
`{100 * summary['z3_line_saving_fraction']:.2f}%` saving. Every candidate and fallback reconstructs
the exact original Lines.

This is a substantial finishing operation, not the missing factor of two. Combining it with the
current `{integrated['structural_wire_bytes']:,}`-byte best structural ledger yields only
`{integrated['cold_equal_corpus_ratio']:.2f}x` equal-corpus and
`{integrated['cold_weighted_ratio']:.2f}x` byte-weighted cold projections. The arithmetic
half-cold projection remains above 200x in aggregate, but only
`{integrated['half_cold_200_pass_corpora']} / 16` individual corpora pass. A real cache scenario is
still required; halving Line wire is not evidence of such a scenario.

## Exact operation

The encoder recognizes only Lines matching this conservative grammar:

```text
horizontal-prefix decimal-u8 (comma fixed-horizontal-separator decimal-u8){{3,}} comma LF
```

Decimal spelling is canonical, every value is in `[0,255]`, every value has a trailing comma, and
the separator is identical within the Line. A frame carries:

```text
BYTE_ARRAY(style table, style-id stream, values-per-Line stream, raw-u8 value stream)
```

All other Lines use the existing lexicographically sorted split-front streams. The receiver renders
decimal digits, commas, exact prefix/separator bytes, and LF, merges the two sorted outputs, and
compares the result with the independent truth set. A one-byte frame selector can encode baseline
versus generated form plus the stream-presence mask. No new message family or predictor is needed.

## Balanced measurements

{markdown_table(('level', 'mode', 'Line wire', 'raw/wire', 'equal-corpus', 'exact'), aggregate_rows)}

The independent row is the product-shaped actual-byte selector over complete per-TU zstd frames.
The two stream rows retain compression state across TUs and are ceilings comparable to the previous
Line report. Each nonempty stream is charged a four-byte length and every generated frame a selector.

The strict subset contains `{sum(row['array_lines'] for row in per_corpus):,}` Lines,
`{sum(row['array_text_bytes'] for row in per_corpus):,}` rendered bytes, and
`{sum(row['array_values'] for row in per_corpus):,}` underlying byte values.

## zstd-3 generated-stream ledger

{markdown_table(('component', 'wire bytes', 'fraction'), component_rows)}

The `{components['array_values']:,}`-byte value stream is the dominant irreducible-looking part of
this operation. Most of Godot's values are already-compressed editor translations, documentation,
fonts, or ICU data. Higher-level source reuse may avoid retransmitting some of them, but ordinary
text prediction should not be expected to manufacture those payload bytes.

## Integrated cold and half-cold projection

| quantity | result |
|---|---:|
| raw `.ii` input | {integrated['raw_bytes']:,} B |
| structural wire | {integrated['structural_wire_bytes']:,} B |
| generated-array Line wire | {integrated['line_wire_bytes']:,} B |
| projected cold wire | {integrated['cold_projection_wire_bytes']:,} B |
| cold ratio, byte-weighted / equal-corpus | {integrated['cold_weighted_ratio']:.2f}x / {integrated['cold_equal_corpus_ratio']:.2f}x |
| corpora at cold 400x | {integrated['cold_400_pass_corpora']} / 16 |
| projected half-cold wire | {integrated['half_cold_projection_wire_bytes']:,.0f} B |
| half-cold ratio, byte-weighted / equal-corpus | {integrated['half_cold_weighted_ratio']:.2f}x / {integrated['half_cold_equal_corpus_ratio']:.2f}x |
| corpora at half-cold 200x | {integrated['half_cold_200_pass_corpora']} / 16 |

{markdown_table(('corpus', 'array text', 'old Line', 'new Line', 'saving', 'cold', 'half-cold*'), corpus_rows)}

`*` Half-cold is the explicit arithmetic projection `structure + 0.5 × cold Line wire`; it is not
an executed cache workload.

Godot supplies almost the entire factor-sized gain: its Line wire falls from `67,067,667` to
`51,870,999` bytes. Small selector/history movements cause sub-kilobyte regressions in six corpora;
an actual-byte TU-local baseline remains available and the aggregate maximum regression is 700
bytes. The design must not special-case the corpus: the strict operation is content-defined and the
literal/front fallback remains authoritative.

## Correctness and speed boundary

- The parser and renderer have accepted/rejected unit cases.
- Every independent baseline, independent candidate, persistent baseline, and persistent candidate
  is decoded from serialized zstd bytes and compared with the exact trace Lines.
- The losing independent representation is also decoded, outside product decode timing, so fallback
  cannot conceal a broken candidate.
- All 96 balanced rows (16 corpora × 2 levels × 3 modes) are exact.
- Python compile, Ruff, and diff checks pass.

The Python research parser is intentionally not a throughput claim. On full Godot it prepares the
272.75 MB first-use Line set in roughly 41 seconds and therefore misses the product gate badly. The
zstd part itself processes the much smaller Line plane quickly, but a C++ parser/renderer must be
measured end to end. Product acceptance remains at least 1 GB/s of original `.ii` input on the real
C/F path, including classification, rendering, compression, framing, and copying.

## Decision and next work

1. Keep `BYTE_ARRAY` as one optional version-dispatched Line-definition representation with ordinary
   fallback; do not create a separate transport.
2. Materialize reconstructed Lines through the ordinary Line installation path for the first product
   row. This isolates the representation gain and preserves existing IDs/composition.
3. Implement the strict scanner and renderer in C++, then benchmark classification plus round trip on
   the complete 16-corpus trace at zstd-1 and zstd-3.
4. Execute actual cold and half-cold cache workloads. The half-cold projection above is useful budget
   arithmetic only.
5. Continue on the remaining 43.12 MB rest-suffix channel and the 46.69 MB structural channel. Flat
   byte phrases are already closed; the next contender must be a coarser parameterized program or a
   simpler structural ledger, always selected by complete serialized bytes.
6. Preserve the existing online learner's encode-before-learn state rule and rerun standard, reverse,
   deterministic shuffles, broad content change, and revert after the Line operation is integrated.

## Reproduction

- Codec input: `{codec_path}`
- Codec SHA-256: `{digest(codec_path)}`
- Structural input: `{structural_path}`
- Structural SHA-256: `{digest(structural_path)}`

```sh
python3 linecache/generated_array_line_codec.py \\
{trace_arguments} \\
  --levels 1 3 \\
  --output {codec_path} \\
  --tsv linecache/ml-artifacts/generated-array-line-program-16corpus-benchmark.tsv

python3 linecache/generated_array_report.py \\
  --codec-report {codec_path} \\
  --structural-summary {structural_path} \\
  --summary linecache/ml-artifacts/generated-array-line-program-16corpus-summary.json \\
  --tsv linecache/ml-artifacts/generated-array-line-program-16corpus.tsv \\
  --output linecache/GENERATED-ARRAY-LINE-PROGRAM-16CORPUS-REPORT.md
```
"""


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--codec-report", required=True)
    value.add_argument("--structural-summary", required=True)
    value.add_argument("--summary", required=True)
    value.add_argument("--tsv", required=True)
    value.add_argument("--output", required=True)
    return value


def main() -> int:
    args = parser().parse_args()
    codec = load_json(args.codec_report)
    structural = load_json(args.structural_summary)
    summary = build_summary(codec, structural)
    Path(args.summary).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    fields = tuple(summary["per_corpus"][0])
    with open(args.tsv, "w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary["per_corpus"])
    Path(args.output).write_text(
        render_report(summary, args.codec_report, args.structural_summary)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
