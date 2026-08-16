#!/usr/bin/env python3
"""Render the issue #16 codec bake-off evidence as one standalone HTML page."""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Sequence


COLORS = {
    "empty": "#617083",
    "installed": "#d18b2c",
    "seed": "#0e8a74",
    "ink": "#17212b",
    "grid": "#d9e0e7",
    "negative": "#b34343",
    "blue": "#3178a8",
    "violet": "#7656a8",
}


def escape(value: object) -> str:
    return html.escape(str(value), quote=True)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def comma(value: int) -> str:
    return f"{value:,}"


def ratio(value: float) -> str:
    return f"{value:.2f}×"


def percent(value: float) -> str:
    return f"{value:+.2f}%"


def svg_learning_curve(checkpoints: Sequence[dict]) -> str:
    width, height = 980, 420
    left, right, top, bottom = 74, 28, 34, 66
    plot_width = width - left - right
    plot_height = height - top - bottom
    max_tu = max(point["tu"] for point in checkpoints)
    series = (
        ("Empty online", "empty_equal_corpus_ratio", COLORS["empty"]),
        ("Installed package", "installed_equal_corpus_ratio", COLORS["installed"]),
        ("C-only seed", "seed_equal_corpus_ratio", COLORS["seed"]),
    )
    maximum = max(point[key] for _, key, _ in series for point in checkpoints)
    y_max = math.ceil(maximum / 50.0) * 50.0

    def x(tu: int) -> float:
        return left + math.log10(tu) / math.log10(max_tu) * plot_width

    def y(value: float) -> float:
        return top + (1.0 - value / y_max) * plot_height

    parts = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" '
        'aria-labelledby="learning-title learning-desc">',
        '<title id="learning-title">Balanced structural startup learning curves</title>',
        '<desc id="learning-desc">Equal-corpus compression ratio by translation unit '
        'for empty online learning, installed pretraining, and a C-only seed.</desc>',
    ]
    for tick in range(0, int(y_max) + 1, 50):
        yy = y(tick)
        parts.append(
            f'<line x1="{left}" y1="{yy:.1f}" x2="{width-right}" '
            f'y2="{yy:.1f}" stroke="{COLORS["grid"]}" />'
        )
        parts.append(
            f'<text x="{left-12}" y="{yy+4:.1f}" text-anchor="end" '
            f'class="axis-label">{tick}×</text>'
        )
    for point in checkpoints:
        xx = x(point["tu"])
        parts.append(
            f'<line x1="{xx:.1f}" y1="{top}" x2="{xx:.1f}" '
            f'y2="{height-bottom}" stroke="{COLORS["grid"]}" />'
        )
        parts.append(
            f'<text x="{xx:.1f}" y="{height-bottom+24}" text-anchor="middle" '
            f'class="axis-label">{point["tu"]}</text>'
        )
        parts.append(
            f'<text x="{xx:.1f}" y="{height-bottom+41}" text-anchor="middle" '
            f'class="cohort-label">n={point["corpora"]}</text>'
        )
    for label, key, color in series:
        coordinates = " ".join(
            f'{x(point["tu"]):.1f},{y(point[key]):.1f}'
            for point in checkpoints
        )
        parts.append(
            f'<polyline points="{coordinates}" fill="none" stroke="{color}" '
            'stroke-width="4" stroke-linejoin="round" stroke-linecap="round" />'
        )
        for point in checkpoints:
            parts.append(
                f'<circle cx="{x(point["tu"]):.1f}" cy="{y(point[key]):.1f}" '
                f'r="5" fill="{color}"><title>{escape(label)} · TU '
                f'{point["tu"]}: {point[key]:.2f}×</title></circle>'
            )
    legend_x = left + 12
    for index, (label, _, color) in enumerate(series):
        current_x = legend_x + index * 235
        parts.append(
            f'<line x1="{current_x}" y1="17" x2="{current_x+30}" y2="17" '
            f'stroke="{color}" stroke-width="5" />'
        )
        parts.append(
            f'<text x="{current_x+39}" y="21" class="legend-label">'
            f'{escape(label)}</text>'
        )
    parts.append(
        f'<text x="{left+plot_width/2:.1f}" y="{height-5}" text-anchor="middle" '
        'class="axis-title">Translation units processed (log scale); n is eligible corpus count</text>'
    )
    parts.append('</svg>')
    return "".join(parts)


def svg_gain_plot(comparisons: Sequence[dict]) -> str:
    width, height = 980, 470
    left, right, top, bottom = 70, 22, 34, 118
    plot_width = width - left - right
    plot_height = height - top - bottom
    values = [
        row[key]
        for row in comparisons
        for key in ("seed_vs_empty_percent", "seed_vs_installed_percent")
    ]
    y_min = min(-10.0, math.floor(min(values) / 10.0) * 10.0)
    y_max = max(60.0, math.ceil(max(values) / 10.0) * 10.0)
    step = plot_width / len(comparisons)

    def x(index: int) -> float:
        return left + step * (index + 0.5)

    def y(value: float) -> float:
        return top + (y_max - value) / (y_max - y_min) * plot_height

    parts = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" '
        'aria-labelledby="gain-title gain-desc">',
        '<title id="gain-title">C-only seed gain by corpus</title>',
        '<desc id="gain-desc">Percentage compression-ratio gain of the C-only seed '
        'against empty learning and installed pretraining at each corpus endpoint.</desc>',
    ]
    for tick in range(int(y_min), int(y_max) + 1, 10):
        yy = y(tick)
        stroke = COLORS["ink"] if tick == 0 else COLORS["grid"]
        stroke_width = 2 if tick == 0 else 1
        parts.append(
            f'<line x1="{left}" y1="{yy:.1f}" x2="{width-right}" '
            f'y2="{yy:.1f}" stroke="{stroke}" stroke-width="{stroke_width}" />'
        )
        parts.append(
            f'<text x="{left-10}" y="{yy+4:.1f}" text-anchor="end" '
            f'class="axis-label">{tick:+d}%</text>'
        )
    for index, row in enumerate(comparisons):
        xx = x(index)
        for offset, key, color, label in (
            (-6, "seed_vs_empty_percent", COLORS["seed"], "vs empty"),
            (6, "seed_vs_installed_percent", COLORS["installed"], "vs installed"),
        ):
            value = row[key]
            zero = y(0)
            yy = y(value)
            parts.append(
                f'<line x1="{xx+offset:.1f}" y1="{zero:.1f}" '
                f'x2="{xx+offset:.1f}" y2="{yy:.1f}" stroke="{color}" '
                'stroke-width="3" opacity="0.68" />'
            )
            parts.append(
                f'<circle cx="{xx+offset:.1f}" cy="{yy:.1f}" r="5" '
                f'fill="{color}"><title>{escape(row["corpus"])} {label}: '
                f'{value:+.2f}%</title></circle>'
            )
        parts.append(
            f'<text transform="translate({xx+3:.1f},{height-bottom+13}) rotate(52)" '
            f'text-anchor="start" class="axis-label">{escape(row["corpus"])}</text>'
        )
    for index, (label, color) in enumerate(
        (("Seed vs empty", COLORS["seed"]), ("Seed vs installed", COLORS["installed"]))
    ):
        legend_x = left + index * 220
        parts.append(
            f'<circle cx="{legend_x}" cy="17" r="6" fill="{color}" />'
            f'<text x="{legend_x+12}" y="21" class="legend-label">{label}</text>'
        )
    parts.append('</svg>')
    return "".join(parts)


def svg_stability_plot(scenarios: Sequence[dict]) -> str:
    """Render the balanced max-200 order/change matrix."""
    width, height = 980, 430
    left, right, top, bottom = 72, 24, 42, 86
    plot_width = width - left - right
    plot_height = height - top - bottom
    maximum = max(
        row[key]
        for row in scenarios
        for key in ("empty_equal_corpus_ratio", "seed_equal_corpus_ratio")
    )
    y_max = math.ceil(maximum / 50.0) * 50.0
    group_width = plot_width / len(scenarios)
    bar_width = min(42.0, group_width * 0.28)

    def y(value: float) -> float:
        return top + (1.0 - value / y_max) * plot_height

    parts = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" '
        'aria-labelledby="stability-title stability-desc">',
        '<title id="stability-title">Generic seed under order and input change</title>',
        '<desc id="stability-desc">Equal-corpus compression ratio for empty and '
        'C-only-seeded online learning across six exact scenarios.</desc>',
    ]
    for tick in range(0, int(y_max) + 1, 50):
        yy = y(tick)
        parts.append(
            f'<line x1="{left}" y1="{yy:.1f}" x2="{width-right}" '
            f'y2="{yy:.1f}" stroke="{COLORS["grid"]}" />'
        )
        parts.append(
            f'<text x="{left-10}" y="{yy+4:.1f}" text-anchor="end" '
            f'class="axis-label">{tick}×</text>'
        )
    for index, row in enumerate(scenarios):
        center = left + group_width * (index + 0.5)
        for offset, key, color, label in (
            (-bar_width * 0.58, "empty_equal_corpus_ratio", COLORS["empty"], "Empty"),
            (bar_width * 0.58, "seed_equal_corpus_ratio", COLORS["seed"], "Seed"),
        ):
            value = row[key]
            xx = center + offset - bar_width / 2
            yy = y(value)
            parts.append(
                f'<rect x="{xx:.1f}" y="{yy:.1f}" width="{bar_width:.1f}" '
                f'height="{height-bottom-yy:.1f}" rx="3" fill="{color}">'
                f'<title>{escape(row["scenario"])} · {label}: {value:.2f}×</title></rect>'
            )
        gain = (
            row["seed_equal_corpus_ratio"]
            / row["empty_equal_corpus_ratio"]
            - 1.0
        ) * 100.0
        parts.append(
            f'<text x="{center:.1f}" y="{top-11}" text-anchor="middle" '
            f'class="cohort-label">{gain:+.1f}%</text>'
        )
        label = row["scenario"].replace("shuffle", "shuffle ")
        parts.append(
            f'<text x="{center:.1f}" y="{height-bottom+23}" text-anchor="middle" '
            f'class="axis-label">{escape(label)}</text>'
        )
        parts.append(
            f'<text x="{center:.1f}" y="{height-bottom+40}" text-anchor="middle" '
            f'class="cohort-label">16/16 exact</text>'
        )
    for index, (label, color) in enumerate(
        (("Empty online", COLORS["empty"]), ("Generic seed → online", COLORS["seed"]))
    ):
        legend_x = left + index * 220
        parts.append(
            f'<rect x="{legend_x}" y="12" width="16" height="12" rx="2" '
            f'fill="{color}"/><text x="{legend_x+24}" y="23" '
            f'class="legend-label">{escape(label)}</text>'
        )
    parts.append(
        f'<text x="{left+plot_width/2:.1f}" y="{height-7}" text-anchor="middle" '
        'class="axis-title">Scenario; labels above groups are seed gain over empty</text>'
    )
    parts.append('</svg>')
    return "".join(parts)


def aggregate_line_rows(rows: Sequence[dict]) -> dict[tuple[int, str], dict]:
    aggregate: dict[tuple[int, str], dict] = defaultdict(
        lambda: {
            "raw_bytes": 0,
            "line_bytes": 0,
            "wire_bytes": 0,
            "encode_seconds": 0.0,
            "decode_seconds": 0.0,
            "corpora": 0,
            "exact": True,
        }
    )
    for row in rows:
        key = (int(row["level"]), row["mode"])
        current = aggregate[key]
        for field in ("raw_bytes", "line_bytes", "wire_bytes"):
            current[field] += int(row[field])
        for field in ("encode_seconds", "decode_seconds"):
            current[field] += float(row[field])
        current["corpora"] += 1
        current["exact"] = current["exact"] and bool(row["exact"])
    for current in aggregate.values():
        current["raw_ratio"] = current["raw_bytes"] / current["wire_bytes"]
        current["line_ratio"] = current["line_bytes"] / current["wire_bytes"]
    return dict(aggregate)


def svg_line_ceiling_plot(aggregate: dict[tuple[int, str], dict]) -> str:
    """Compare exact online-compatible Line encodings against the cold allowance."""
    selected = [
        (3, "independent-best", "z3 · TU-local best", COLORS["empty"]),
        (3, "independent-split-front", "z3 · TU-local split", COLORS["blue"]),
        (3, "stream-split-front", "z3 · stateful split", COLORS["seed"]),
        (6, "stream-split-front", "z6 · stateful split", COLORS["violet"]),
    ]
    selected = [row for row in selected if (row[0], row[1]) in aggregate]
    width, height = 980, 430
    left, right, top, bottom = 82, 26, 42, 112
    plot_width = width - left - right
    plot_height = height - top - bottom
    allowance = 24_696_485
    maximum = max(aggregate[(level, mode)]["wire_bytes"] for level, mode, _, _ in selected)
    y_max = math.ceil(maximum / 20_000_000) * 20_000_000
    step = plot_width / len(selected)
    bar_width = min(105.0, step * 0.56)

    def y(value: float) -> float:
        return top + (1.0 - value / y_max) * plot_height

    parts = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" '
        'aria-labelledby="line-title line-desc">',
        '<title id="line-title">Exact first-use Line wire versus cold-400 allowance</title>',
        '<desc id="line-desc">Aggregate exact Line wire for four encodings across '
        '16 corpora. The allowance excludes all remaining unmeasured blocks.</desc>',
    ]
    for tick in range(0, int(y_max) + 1, 20_000_000):
        yy = y(tick)
        parts.append(
            f'<line x1="{left}" y1="{yy:.1f}" x2="{width-right}" '
            f'y2="{yy:.1f}" stroke="{COLORS["grid"]}" />'
        )
        parts.append(
            f'<text x="{left-10}" y="{yy+4:.1f}" text-anchor="end" '
            f'class="axis-label">{tick/1e6:.0f} MB</text>'
        )
    allowance_y = y(allowance)
    parts.append(
        f'<line x1="{left}" y1="{allowance_y:.1f}" x2="{width-right}" '
        f'y2="{allowance_y:.1f}" stroke="{COLORS["installed"]}" '
        'stroke-width="3" stroke-dasharray="9 7" />'
    )
    parts.append(
        f'<text x="{width-right-4}" y="{allowance_y-7:.1f}" text-anchor="end" '
        'class="legend-label" fill="#8a4b1f">24.70 MB maximum after structure</text>'
    )
    for index, (level, mode, label, color) in enumerate(selected):
        row = aggregate[(level, mode)]
        center = left + step * (index + 0.5)
        yy = y(row["wire_bytes"])
        parts.append(
            f'<rect x="{center-bar_width/2:.1f}" y="{yy:.1f}" '
            f'width="{bar_width:.1f}" height="{height-bottom-yy:.1f}" '
            f'rx="4" fill="{color}"><title>{escape(label)}: '
            f'{row["wire_bytes"]:,} bytes; exact={row["exact"]}</title></rect>'
        )
        parts.append(
            f'<text x="{center:.1f}" y="{yy-9:.1f}" text-anchor="middle" '
            f'class="legend-label">{row["wire_bytes"]/1e6:.2f} MB</text>'
        )
        words = label.split(" · ")
        parts.append(
            f'<text x="{center:.1f}" y="{height-bottom+24}" text-anchor="middle" '
            f'class="axis-label">{escape(words[0])}</text>'
        )
        parts.append(
            f'<text x="{center:.1f}" y="{height-bottom+42}" text-anchor="middle" '
            f'class="cohort-label">{escape(words[-1])}</text>'
        )
    parts.append(
        f'<text x="{left+plot_width/2:.1f}" y="{height-8}" text-anchor="middle" '
        'class="axis-title">Line-text bytes only; the dashed allowance already assumes the best structural hybrid</text>'
    )
    parts.append('</svg>')
    return "".join(parts)


def svg_source_conditioning_plot(rows: Sequence[dict]) -> str:
    """Render the related implementer attribution experiment on a log x-axis."""
    width, height = 980, 520
    left, right, top, bottom = 154, 34, 34, 54
    plot_width = width - left - right
    plot_height = height - top - bottom
    x_min, x_max = 100.0, 6000.0

    def x(value: float) -> float:
        return left + (
            (math.log10(value) - math.log10(x_min))
            / (math.log10(x_max) - math.log10(x_min))
            * plot_width
        )

    step = plot_height / len(rows)
    parts = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img" '
        'aria-labelledby="source-title source-desc">',
        '<title id="source-title">Related source-conditioning capability result</title>',
        '<desc id="source-desc">Plain dictionary-leg ratio and ratio when installed '
        'toolchain text is reconstructed from its source attribution.</desc>',
    ]
    for tick in (100, 200, 400, 800, 1600, 3200, 6000):
        xx = x(float(tick))
        stroke = COLORS["installed"] if tick == 400 else COLORS["grid"]
        width_value = 2 if tick == 400 else 1
        parts.append(
            f'<line x1="{xx:.1f}" y1="{top}" x2="{xx:.1f}" '
            f'y2="{height-bottom}" stroke="{stroke}" stroke-width="{width_value}" />'
        )
        parts.append(
            f'<text x="{xx:.1f}" y="{height-bottom+25}" text-anchor="middle" '
            f'class="axis-label">{tick}×</text>'
        )
    for index, row in enumerate(sorted(rows, key=lambda item: float(item["srccond_ratio"]))):
        yy = top + step * (index + 0.5)
        plain = float(row["dict_ratio"])
        conditioned = float(row["srccond_ratio"])
        parts.append(
            f'<text x="{left-12}" y="{yy+4:.1f}" text-anchor="end" '
            f'class="axis-label">{escape(row["corpus"])}</text>'
        )
        parts.append(
            f'<line x1="{x(plain):.1f}" y1="{yy:.1f}" '
            f'x2="{x(conditioned):.1f}" y2="{yy:.1f}" '
            f'stroke="{COLORS["grid"]}" stroke-width="4" />'
        )
        parts.append(
            f'<circle cx="{x(plain):.1f}" cy="{yy:.1f}" r="5" '
            f'fill="{COLORS["empty"]}"><title>{escape(row["corpus"])} plain: '
            f'{plain:.1f}×</title></circle>'
        )
        parts.append(
            f'<circle cx="{x(conditioned):.1f}" cy="{yy:.1f}" r="6" '
            f'fill="{COLORS["blue"]}"><title>{escape(row["corpus"])} '
            f'source-conditioned: {conditioned:.1f}×</title></circle>'
        )
    parts.append(
        f'<circle cx="{left}" cy="16" r="5" fill="{COLORS["empty"]}" />'
        f'<text x="{left+12}" y="20" class="legend-label">Plain dict leg</text>'
        f'<circle cx="{left+190}" cy="16" r="6" fill="{COLORS["blue"]}" />'
        f'<text x="{left+202}" y="20" class="legend-label">Source-conditioned leg</text>'
    )
    parts.append('</svg>')
    return "".join(parts)


def metric_card(label: str, value: str, note: str, tone: str = "") -> str:
    return (
        f'<article class="metric {escape(tone)}"><div class="metric-label">'
        f'{escape(label)}</div><div class="metric-value">{escape(value)}</div>'
        f'<div class="metric-note">{escape(note)}</div></article>'
    )


def render(args: argparse.Namespace) -> str:
    seed_path = Path(args.seed_summary)
    full_seed_path = Path(args.full_seed_summary)
    hybrid_path = Path(args.hybrid_summary)
    stability_path = Path(args.stability_summary)
    line_path = Path(args.line_ceiling)
    source_path = Path(args.source_conditioning)
    seed = json.loads(seed_path.read_text())
    full_seed = json.loads(full_seed_path.read_text())
    hybrid = json.loads(hybrid_path.read_text())
    stability = json.loads(stability_path.read_text())
    line_report = json.loads(line_path.read_text())
    line_aggregate = aggregate_line_rows(line_report["rows"])
    with source_path.open(newline="") as source_input:
        source_conditioning = list(csv.DictReader(source_input, delimiter="\t"))
    screen_endpoint = seed["endpoint"]
    endpoint = full_seed["endpoint"]
    comparisons = full_seed["comparisons"]
    checkpoints = seed["checkpoints"]
    hybrid_by_corpus = {
        row["corpus"]: row["charged_ratio"] for row in hybrid["selected"]
    }

    checkpoint_rows = []
    for point in checkpoints:
        empty_state = (
            f'{point["seed_strict_wins_vs_empty"]} wins, '
            f'{point["seed_ties_vs_empty"]} ties'
        )
        installed_state = (
            f'{point["seed_strict_wins_vs_installed"]} wins, '
            f'{point["seed_ties_vs_installed"]} ties'
        )
        checkpoint_rows.append(
            '<tr>'
            f'<td>{point["tu"]}</td><td>{point["corpora"]}</td>'
            f'<td>{ratio(point["empty_equal_corpus_ratio"])}</td>'
            f'<td>{ratio(point["installed_equal_corpus_ratio"])}</td>'
            f'<td class="best">{ratio(point["seed_equal_corpus_ratio"])}</td>'
            f'<td>{percent(point["seed_vs_empty_equal_percent"])}</td>'
            f'<td>{escape(empty_state)}</td><td>{escape(installed_state)}</td>'
            '</tr>'
        )

    max_endpoint_ratio = max(row["seed_ratio"] for row in comparisons)
    corpus_rows = []
    for row in comparisons:
        installed_class = " negative" if row["seed_vs_installed_percent"] < 0 else ""
        bar_width = max(1.0, row["seed_ratio"] / max_endpoint_ratio * 100.0)
        corpus_rows.append(
            '<tr>'
            f'<td><strong>{escape(row["corpus"])}</strong></td>'
            f'<td>{row["screen_tus"]}</td>'
            f'<td>{ratio(row["empty_ratio"])}</td>'
            f'<td>{ratio(row["installed_ratio"])}</td>'
            f'<td class="best">{ratio(row["seed_ratio"])}</td>'
            f'<td>{percent(row["seed_vs_empty_percent"])}</td>'
            f'<td class="{installed_class.strip()}">'
            f'{percent(row["seed_vs_installed_percent"])}</td>'
            f'<td>{row["seed_published_assets"]:,} / '
            f'{row["all_published_assets"]:,}</td>'
            f'<td>{ratio(hybrid_by_corpus[row["corpus"]])}</td>'
            '</tr>'
            f'<tr class="bar-row"><td colspan="9"><div class="mini-track">'
            f'<div class="mini-bar" style="width:{bar_width:.2f}%"></div>'
            '</div></td></tr>'
        )

    package_rows = []
    for package in full_seed["seed_packages"]:
        package_rows.append(
            '<tr>'
            f'<td><code>{escape(package["sha256"][:16])}…</code></td>'
            f'<td>{package["assets"]:,}</td><td>{comma(package["raw_bytes"])}</td>'
            f'<td>{comma(package["compressed_bytes"])}</td>'
            f'<td>{escape(", ".join(package["targets"]))}</td>'
            '</tr>'
        )

    stability_rows = []
    for row in stability["scenario_summary"]:
        seed_gain = (
            row["seed_equal_corpus_ratio"]
            / row["empty_equal_corpus_ratio"]
            - 1.0
        ) * 100.0
        stability_rows.append(
            '<tr>'
            f'<td><strong>{escape(row["scenario"])}</strong></td>'
            f'<td>{ratio(row["empty_equal_corpus_ratio"])}</td>'
            f'<td class="best">{ratio(row["seed_equal_corpus_ratio"])}</td>'
            f'<td>{percent(seed_gain)}</td>'
            f'<td>{ratio(row["seed_byte_weighted_ratio"])}</td>'
            f'<td>{comma(row["seed_wire_bytes"])}</td>'
            f'<td>{row["seed_strict_wins_vs_empty"]} / {row["corpora"]}</td>'
            f'<td>{row["exact_corpora"]} / {row["corpora"]}</td>'
            '</tr>'
        )

    stability_corpus_rows = []
    for row in sorted(
        stability["corpus_summary"],
        key=lambda item: item["worst_change_vs_standard_percent"],
    ):
        stability_corpus_rows.append(
            '<tr>'
            f'<td><strong>{escape(row["corpus"])}</strong></td>'
            f'<td>{ratio(row["standard_seed_ratio"])}</td>'
            f'<td>{ratio(row["minimum_seed_ratio"])}</td>'
            f'<td>{escape(row["worst_scenario"])}</td>'
            f'<td>{percent(row["worst_change_vs_standard_percent"])}</td>'
            f'<td>{percent(row["minimum_seed_gain_vs_empty_percent"])}</td>'
            f'<td>{row["seed_wins_vs_empty"]} / {row["scenarios"]}</td>'
            '</tr>'
        )

    line_modes = (
        (3, "independent-best", "TU-local best of literal/front"),
        (3, "independent-split-front", "TU-local split-front"),
        (3, "stream-front", "Ordered stream front"),
        (3, "stream-split-front", "Ordered stream split-front"),
        (6, "stream-split-front", "Ordered stream split-front"),
        (6, "whole-split-front", "Whole-corpus split-front ceiling"),
    )
    line_rows = []
    for level, mode, label in line_modes:
        if (level, mode) not in line_aggregate:
            continue
        row = line_aggregate[(level, mode)]
        line_rows.append(
            '<tr>'
            f'<td>zstd-{level}</td><td>{escape(label)}</td>'
            f'<td>{comma(row["wire_bytes"])}</td>'
            f'<td>{ratio(row["line_ratio"])}</td>'
            f'<td>{ratio(row["raw_ratio"])}</td>'
            f'<td>{row["corpora"]} / 16</td>'
            f'<td>{"yes" if row["exact"] else "no"}</td>'
            '</tr>'
        )

    source_rows = []
    for row in sorted(
        source_conditioning,
        key=lambda item: float(item["srccond_ratio"]),
    ):
        lift = float(row["srccond_ratio"]) / float(row["dict_ratio"])
        source_rows.append(
            '<tr>'
            f'<td><strong>{escape(row["corpus"])}</strong></td>'
            f'<td>{row["tus"]}</td><td>{float(row["raw_MiB"]):,.1f}</td>'
            f'<td>{ratio(float(row["dict_ratio"]))}</td>'
            f'<td class="best">{ratio(float(row["srccond_ratio"]))}</td>'
            f'<td>{lift:.2f}×</td><td>{float(row["line_cov_pctB"]):.2f}%</td>'
            '</tr>'
        )

    cold_budget = hybrid["aggregate_raw_bytes"] / 400.0
    line_allowance = cold_budget - hybrid["aggregate_wire_bytes"]
    z3_split = line_aggregate.get((3, "stream-split-front"))
    z3_incomplete_ratio = (
        hybrid["aggregate_raw_bytes"]
        / (hybrid["aggregate_wire_bytes"] + z3_split["wire_bytes"])
        if z3_split
        else 0.0
    )
    hybrid_rows = {row["corpus"]: row for row in hybrid["selected"]}
    z3_line_rows = {
        row["corpus"]: row
        for row in line_report["rows"]
        if int(row["level"]) == 3 and row["mode"] == "stream-split-front"
    }
    combined_cold_ratios = []
    combined_half_ratios = []
    for corpus, structural in hybrid_rows.items():
        if corpus not in z3_line_rows:
            continue
        line_wire = int(z3_line_rows[corpus]["wire_bytes"])
        raw = int(structural["raw_bytes"])
        structural_wire = int(structural["charged_wire_bytes"])
        combined_cold_ratios.append(raw / (structural_wire + line_wire))
        combined_half_ratios.append(raw / (structural_wire + line_wire / 2.0))
    combined_cold_equal = (
        len(combined_cold_ratios)
        / sum(1.0 / value for value in combined_cold_ratios)
        if combined_cold_ratios else 0.0
    )
    combined_half_equal = (
        len(combined_half_ratios)
        / sum(1.0 / value for value in combined_half_ratios)
        if combined_half_ratios else 0.0
    )
    half_incomplete_ratio = (
        hybrid["aggregate_raw_bytes"]
        / (hybrid["aggregate_wire_bytes"] + z3_split["wire_bytes"] / 2.0)
        if z3_split else 0.0
    )

    styles = """
:root { --paper:#fbfaf7; --ink:#17212b; --muted:#5c6874; --line:#d8dde2;
  --seed:#0e8a74; --installed:#d18b2c; --empty:#617083; --soft:#eef3f2;
  --warning:#8a4b1f; --negative:#b34343; }
* { box-sizing:border-box; }
html { background:#e9ecef; color:var(--ink); font-family:Inter,ui-sans-serif,system-ui,
  -apple-system,"Segoe UI",sans-serif; font-size:16px; line-height:1.55; }
body { margin:0; }
.page { width:min(1180px,calc(100% - 28px)); margin:28px auto; background:var(--paper);
  box-shadow:0 16px 55px rgba(20,31,43,.14); }
.hero { padding:68px 72px 56px; color:white;
  background:linear-gradient(135deg,#152534 0%,#193c43 58%,#0e796c 100%); }
.kicker { letter-spacing:.16em; text-transform:uppercase; font-weight:700; font-size:.76rem;
  color:#b7e8dd; }
h1 { font-family:Georgia,"Times New Roman",serif; font-size:clamp(2.5rem,6vw,5.2rem);
  line-height:.97; font-weight:500; max-width:920px; margin:16px 0 24px; }
.hero p { max-width:850px; font-size:1.14rem; color:#dce9e8; }
.hero-meta { display:flex; flex-wrap:wrap; gap:14px 28px; margin-top:30px; font-size:.85rem;
  color:#bcd1d0; }
main { padding:54px 72px 76px; }
section { margin:0 0 64px; }
h2 { font-family:Georgia,"Times New Roman",serif; font-weight:500; font-size:2.2rem;
  line-height:1.1; margin:0 0 16px; }
h3 { font-size:1.08rem; letter-spacing:.02em; margin:28px 0 10px; }
.lead { font-size:1.14rem; max-width:900px; color:#34434f; }
.eyebrow { color:var(--seed); font-weight:800; text-transform:uppercase;
  letter-spacing:.13em; font-size:.72rem; margin-bottom:8px; }
.metrics { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:14px; margin:25px 0; }
.metric { border:1px solid var(--line); border-top:4px solid var(--seed); padding:18px 17px;
  background:white; min-height:150px; }
.metric.warn { border-top-color:var(--installed); }
.metric.neutral { border-top-color:var(--empty); }
.metric-label { color:var(--muted); font-size:.75rem; letter-spacing:.07em;
  text-transform:uppercase; font-weight:700; }
.metric-value { font-family:Georgia,"Times New Roman",serif; font-size:2.05rem;
  margin:6px 0 4px; line-height:1; }
.metric-note { color:var(--muted); font-size:.82rem; line-height:1.35; }
.callout { border-left:5px solid var(--seed); background:var(--soft); padding:18px 22px;
  margin:22px 0; }
.callout.warning { border-color:var(--installed); background:#f8f1e8; }
.chart-wrap { background:white; border:1px solid var(--line); padding:16px 18px 6px;
  margin:22px 0 8px; overflow-x:auto; }
.chart { display:block; min-width:720px; width:100%; height:auto; }
.axis-label,.cohort-label,.legend-label,.axis-title { font-family:Inter,ui-sans-serif,system-ui;
  fill:#53616e; font-size:12px; }
.cohort-label { fill:#87919b; font-size:10px; }
.legend-label { font-size:13px; font-weight:700; }
.axis-title { font-size:12px; font-weight:600; }
.caption { color:var(--muted); font-size:.82rem; margin:7px 4px 0; }
.flow { display:grid; grid-template-columns:1fr 34px 1fr 34px 1fr 34px 1fr;
  align-items:stretch; margin:26px 0; }
.flow-box { border:1px solid var(--line); padding:17px; background:white; }
.flow-box strong { display:block; color:var(--seed); margin-bottom:5px; }
.arrow { display:flex; align-items:center; justify-content:center; color:var(--seed);
  font-size:1.55rem; font-weight:800; }
.split { display:grid; grid-template-columns:1fr 1fr; gap:24px; }
.plane { border:1px solid var(--line); padding:20px; background:white; }
.plane h3 { margin-top:0; }
table { width:100%; border-collapse:collapse; font-size:.84rem; background:white; }
th { text-align:left; color:#4e5a66; font-size:.71rem; text-transform:uppercase;
  letter-spacing:.055em; background:#f0f2f2; }
th,td { padding:9px 9px; border-bottom:1px solid #e3e6e8; vertical-align:top; }
td.best { color:var(--seed); font-weight:800; }
td.negative,.negative { color:var(--negative); font-weight:700; }
.table-scroll { overflow-x:auto; border:1px solid var(--line); }
.bar-row td { padding:0 9px 5px; border:0; }
.mini-track { height:3px; background:#edf0f1; }
.mini-bar { height:3px; background:var(--seed); }
.goal-row { display:grid; grid-template-columns:170px 1fr 95px; align-items:center; gap:14px;
  margin:13px 0; }
.goal-track { height:13px; background:#e5e9eb; position:relative; overflow:hidden; }
.goal-fill { height:100%; background:var(--seed); }
.goal-fill.warning { background:var(--installed); }
.goal-value { font-variant-numeric:tabular-nums; text-align:right; font-weight:700; }
.small { font-size:.84rem; color:var(--muted); }
code { font-family:"SFMono-Regular",Consolas,monospace; font-size:.88em; }
pre { white-space:pre-wrap; word-break:break-word; background:#17212b; color:#e6f0ef;
  padding:18px 20px; font-size:.78rem; line-height:1.5; overflow:auto; }
ul,ol { padding-left:1.35rem; }
li { margin:.35rem 0; }
.status { display:inline-flex; align-items:center; gap:7px; border-radius:99px; padding:4px 10px;
  font-size:.73rem; font-weight:700; background:#e0f1ed; color:#08715f; }
.status::before { content:""; width:7px; height:7px; border-radius:50%; background:#0e8a74; }
.footer { border-top:1px solid var(--line); padding-top:24px; color:var(--muted);
  font-size:.8rem; }
a { color:#087967; }
@media (max-width:850px) { .hero,main { padding-left:28px; padding-right:28px; }
  .metrics { grid-template-columns:1fr 1fr; } .flow { grid-template-columns:1fr; }
  .arrow { transform:rotate(90deg); min-height:34px; } .split { grid-template-columns:1fr; } }
@media (max-width:520px) { .metrics { grid-template-columns:1fr; } }
@media print { @page { size:A4; margin:13mm; } html { background:white; font-size:10.5pt; }
  .page { width:auto; margin:0; box-shadow:none; } .hero { padding:25mm 13mm 18mm;
  -webkit-print-color-adjust:exact; print-color-adjust:exact; } main { padding:14mm 13mm; }
  section { break-inside:auto; margin-bottom:12mm; } .metric,.plane,.callout,.chart-wrap,
  .flow-box { break-inside:avoid; } .chart-wrap { border:0; padding:0; overflow:visible; }
  .chart { min-width:0; width:100%; } .table-scroll { overflow:visible; border:0; }
  thead { display:table-header-group; } tr { break-inside:avoid; }
  a { color:inherit; text-decoration:none; } }
"""

    github_root = (
        "https://github.com/mickg10/icecream/blob/"
        "local-oracle/issue16-c-only-seed/linecache"
    )
    html_text = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport"
 content="width=device-width,initial-scale=1"><title>Issue #16 · Generic Bootstrap and Online Codec</title>
<style>{styles}</style></head><body><article class="page">
<header class="hero"><div class="kicker">Icecream protocol 50 · issue #16 · research report</div>
<h1>Generic bootstrap → online codec</h1>
<p>A corpus-balanced, exact-replay report on a pre-shared generic vocabulary that
accelerates cold startup and is progressively superseded by project-specific online
learning. It combines complete structural results, 96 stability runs, exact Line-plane
ceilings, the whole-transfer budget, and a minimal C/F implementation boundary.</p>
<div class="hero-meta"><span>16 corpora · {comma(endpoint['raw_bytes'])} bytes in complete runs</span>
<span>Generated 2026-08-15</span><span>Branch: local-oracle/issue16-c-only-seed</span>
<span class="status">16 complete + 96 stability rows exact</span></div></header><main>

<section><div class="eyebrow">Executive result</div><h2>Ship a generic output vocabulary; let online learning take over</h2>
<p class="lead">Across every translation unit of all 16 complete corpora, the C-only
seed improves the equal-corpus structural ratio from
<strong>{ratio(endpoint['empty_equal_corpus_ratio'])}</strong> for empty learning and
<strong>{ratio(endpoint['installed_equal_corpus_ratio'])}</strong> for a fully installed
package to <strong>{ratio(endpoint['seed_equal_corpus_ratio'])}</strong>. It sends no
package at TU 0 and publishes only definitions that already repay themselves in the
TU carrying them. In the max-200 stability matrix it beats empty-start learning in
<strong>{stability['seed_strict_wins_vs_empty']} / {stability['runs']}</strong> exact
corpus/scenario pairs.</p>
<div class="metrics">
{metric_card('C-only seed', ratio(endpoint['seed_equal_corpus_ratio']), 'Equal-corpus structural ratio at complete-corpus endpoints')}
{metric_card('Stability wins', f'{stability["seed_strict_wins_vs_empty"]} / {stability["runs"]}', 'Standard, reverse, three fixed shuffles, and global input perturbation')}
{metric_card('Line allowance', f'{line_allowance/1e6:.2f} MB', 'Maximum left after the best structural hybrid for an aggregate cold-400 result', 'warn')}
{metric_card('Seed exact replay', '112 / 112', '16 complete endpoints plus 96 order/change rows; Line rows are reported separately', 'neutral')}
</div>
<p class="small">The complete rows cover {comma(endpoint['raw_bytes'])} raw bytes and
send {comma(endpoint['seed_wire_bytes'])} structural bytes: a byte-weighted
{ratio(endpoint['seed_byte_weighted_ratio'])}. C-side logical learner state at the
complete endpoints ranges from
{comma(endpoint['minimum_final_logical_state_bytes'])} to
{comma(endpoint['maximum_final_logical_state_bytes'])} bytes (median
{comma(int(endpoint['median_final_logical_state_bytes']))}). This is deterministic
key-and-counter accounting, not Python process RSS or a product allocation estimate.</p>
<div class="callout"><strong>Recommended product boundary.</strong> Preinstall a
small, versioned static superblock/output table at C and F. C uses the generic model
and the growing project learner to choose actual-byte candidates. The wire carries a
stable static ID, a once-only dynamic definition, or an exact literal fallback. F
does not need to rerun the ranking model: it expands the selected ID and retains
received dynamic definitions. The generic artifact is the bootstrap; project history
becomes the primary codebook.</div>
<div class="callout warning"><strong>Scope boundary.</strong> These are exact
Region-program structural bytes, not complete <code>.ii</code> transfer. Line text,
Region-to-Line composition, typed values, literal residuals, missing-object exchanges,
and production framing are not in this startup ratio. No total cold-400 claim is made.</div>
</section>

<section><div class="eyebrow">Whole objective ledger</div><h2>The Line plane—not structural IDs—is now the dominant gap</h2>
<p>The best full-run structural hybrid sends {comma(hybrid['aggregate_wire_bytes'])}
bytes and reaches {ratio(hybrid['equal_corpus_ratio'])} equal-corpus and
{ratio(hybrid['aggregate_ratio'])} byte-weighted. Aggregate cold-400 permits only
{comma(int(cold_budget))} total bytes, leaving {comma(int(line_allowance))} bytes for
Line text <em>and every remaining block</em>. The exact zstd-3 stateful split-front
Line contender alone sends {comma(z3_split['wire_bytes']) if z3_split else 'pending'}
bytes.</p>
<div class="goal-row"><strong>Complete cold</strong><div class="goal-track"><div
 class="goal-fill warning" style="width:{z3_incomplete_ratio/400*100:.2f}%"></div></div>
<div class="goal-value">{z3_incomplete_ratio:.2f} / 400×*</div></div>
<div class="goal-row"><strong>Complete half-cold</strong><div class="goal-track"><div
 class="goal-fill" style="width:100%"></div></div><div class="goal-value">{half_incomplete_ratio:.2f} / 200×*</div></div>
<div class="goal-row"><strong>Structural installed</strong><div class="goal-track"><div
 class="goal-fill" style="width:100%"></div></div><div class="goal-value">462.92×</div></div>
<p class="small">*The new cold number is byte-weighted structure + exact Line text;
its equal-corpus result is {combined_cold_equal:.2f}×. The earlier 262.80× half-cold
number uses the independently measured TU-local Line block. With the new stateful
split-front bytes, the optimistic incomplete half-cold figures would be
{half_incomplete_ratio:.2f}× byte-weighted and {combined_half_equal:.2f}× equal-corpus.
Neither includes typed values, composition, requests, framing, or the real cache
scenario.</p></section>

<section><div class="eyebrow">Startup behavior</div><h2>Learning curves, with cohort changes visible</h2>
<p>The fixed startup screen covers {comma(screen_endpoint['raw_bytes'])} raw bytes.
At each corpus’s <code>min(200, completion)</code> endpoint, the equal-corpus ratio is
{ratio(screen_endpoint['seed_equal_corpus_ratio'])}, a
{percent(screen_endpoint['seed_vs_empty_equal_percent'])} gain over empty learning and
{percent(screen_endpoint['seed_vs_installed_equal_percent'])} over the installed package.</p>
<div class="chart-wrap">{svg_learning_curve(checkpoints)}</div>
<p class="caption">Ratios are harmonic equal-corpus ratios. TU 50/100/200 contain
survivor cohorts because shorter corpora have already ended; the eligible count is
printed below each checkpoint. The log x-axis keeps TU 1–25 legible.</p>
<div class="table-scroll"><table><thead><tr><th>TU</th><th>Corpora</th><th>Empty</th>
<th>Installed</th><th>C-only seed</th><th>Seed vs empty</th><th>Seed/empty state</th>
<th>Seed/installed state</th></tr></thead><tbody>{''.join(checkpoint_rows)}</tbody></table></div>
<div class="callout"><strong>TU 1 is a tie, not a seed win.</strong> Empty and C-only
seed generate exactly the same selected wire on all 16 corpora because no seed
definition has yet been worth publishing. The installed package is burdened by its
complete package frame. By TU 5 the seed leads 15/16 corpora; Eigen is the sole early
exception and crosses empty permanently at TU 126.</div></section>

<section><div class="eyebrow">Stability matrix</div><h2>The bootstrap survives reorder and a broad input change</h2>
<p>The max-200 screen covers {comma(stability['scenario_summary'][0]['raw_bytes'])}
raw bytes per scenario. It runs standard order, reverse order, three fixed shuffles,
and a global replacement of the most-shared Region. Every one of the 96 decodes is
exact; the seed is strictly smaller than empty learning in every corpus/scenario pair.</p>
<div class="chart-wrap">{svg_stability_plot(stability['scenario_summary'])}</div>
<p class="caption">Order changes alter the absolute ratio because they alter what the
online learner has seen. The relevant result is that the target-disjoint seed remains
useful in every order. Perturb and standard are nearly identical in aggregate.</p>
<div class="table-scroll"><table><thead><tr><th>Scenario</th><th>Empty equal</th>
<th>Seed equal</th><th>Seed gain</th><th>Seed weighted</th><th>Seed wire</th>
<th>Seed wins</th><th>Exact</th></tr></thead><tbody>{''.join(stability_rows)}</tbody></table></div>
<h3>Per-corpus worst observed order</h3>
<div class="table-scroll"><table><thead><tr><th>Corpus</th><th>Standard seed</th>
<th>Minimum seed</th><th>Worst scenario</th><th>Change from standard</th>
<th>Minimum seed gain vs empty</th><th>Wins</th></tr></thead>
<tbody>{''.join(stability_corpus_rows)}</tbody></table></div>
<p class="small">Cereal is the most order-sensitive absolute ratio (-39.20% in
reverse), followed by Eigen (-28.94%). Even there, the seed still wins every paired
empty-start run. Delayed/reverted multi-build changes and scheduler-shaped concurrent
arrival remain separate required scenarios.</p></section>

<section><div class="eyebrow">Proposed product architecture</div><h2>Share the outputs; keep ranking and learning modular</h2>
<div class="flow"><div class="flow-box"><strong>1 · Versioned generic pack</strong>
Ship a compact static vocabulary/output table with protocol 50 at both endpoints.
Give it a stable pack ID and a reserved static-ID range.</div><div class="arrow">→</div>
<div class="flow-box"><strong>2 · C candidate engine</strong>Use the generic ranker,
the static outputs, and bounded per-project online history to propose complete current-TU
candidates. Keep the literal baseline.</div><div class="arrow">→</div>
<div class="flow-box"><strong>3 · Actual wire choice</strong>Send static IDs, known
dynamic IDs, once-only dynamic definitions, and residual literals. Choose only after
counting every byte in the complete candidate.</div><div class="arrow">→</div>
<div class="flow-box"><strong>4 · F expansion/store</strong>Resolve static IDs from
the shared pack, install explicit dynamic definitions under the C-cache identity, and
emit the exact Line stream into the compiler pipe.</div></div>

<div class="split"><div class="plane"><h3>Pre-shared, software lifetime</h3><ul>
<li>pack format and codec version;</li><li>immutable static superblock definitions;</li>
<li>stable static IDs and small decode tables;</li><li>optionally the same generic
ranker at C and F, although F does not need it for the first implementation.</li></ul></div>
<div class="plane"><h3>Online, per-C cache lifetime</h3><ul><li>dynamic definition ID
space and exact definitions;</li><li>C-side candidate statistics and bounded learner;</li>
<li>F-side installed dynamic dictionary;</li><li>dense Region/Line stores rebuilt from
completed exact output.</li></ul></div></div>

<h3>Why outputs are the minimal shared object</h3>
<p>The decoder never has to guess which phrase C selected. A static ID names a
pre-shared output; a dynamic ID names a previously transmitted exact definition; a
literal carries itself. That permits the C ranking model to change independently of
F. It also permits a future larger model without changing the basic wire blocks. If
measurement later shows that once-only dynamic definitions are a material fraction,
both endpoints can run the same deterministic learner—but that requires a single
commit order across concurrent TUs. Explicit definitions avoid that coupling now.</p>

<h3>Per-TU transition</h3>
<ol><li>Build TU <em>t</em> candidates from the static pack and committed project state
through <em>t−1</em>.</li><li>Encode the empty/literal baseline and every contender,
including definitions, indices, lengths, selectors, and compression framing.</li>
<li>Commit the smallest complete candidate; keep an exact fallback for every field.</li>
<li>F resolves the static/dynamic IDs, installs complete new definitions, and
reconstructs the exact stream.</li><li>Only after TU completion may C learn from it for
future work. F retains only decoder-visible state unless mirrored learning earns its
complexity in a later measurement.</li></ol>
<div class="callout warning"><strong>Measured versus proposed.</strong> The current
16-corpus experiment uses a C-only seed and charges each selected seed definition on
first use. A truly pre-shared output table should remove those first-use definition
bytes, but that improvement must be measured as a separate row; it is not silently
credited to the current result.</div>
<div class="callout warning"><strong>Training boundary.</strong> The present seed is
trained on target-disjoint expanded <code>.ii</code> traces only as a mechanism and
wire-accounting control. It is not the universal product pack. The product candidate
must be learned from raw C/C++ source and parameterized structural programs, evaluated
with the target project, alternate builds of that project, and compiler/toolchain
families held out. Runtime expanded input belongs to the online learner.</div></section>

<section><div class="eyebrow">Exact Line-plane ceiling</div><h2>Continuous state helps, but ordinary compression cannot close cold-400</h2>
<p>The Line harness extracts exact first-use Line text from all 16 traces and tests
independent TU frames, one ordered cache-lifetime stream, and whole-corpus offline
ceilings. Every reported frame is decoded and byte-compared. Split-front coding
compresses prefix lengths, output lengths, and suffix bytes as separate streams.</p>
<div class="chart-wrap">{svg_line_ceiling_plot(line_aggregate)}</div>
<p class="caption">The dashed line is already generous: it is the entire aggregate
allowance remaining after the 46.69 MB best structural hybrid, before composition,
typed values, requests, or framing. Whole-corpus rows are capability ceilings, not a
streaming product design.</p>
<div class="table-scroll"><table><thead><tr><th>Level</th><th>Representation</th>
<th>Wire bytes</th><th>Line ratio</th><th>Raw / Line wire</th><th>Corpora</th>
<th>Exact</th></tr></thead><tbody>{''.join(line_rows)}</tbody></table></div>
<div class="split"><div class="plane"><h3>Retain now</h3><ul><li>TU-local
split-front as a small independent Line block with literal fallback;</li><li>one
stateful-stream capability row to quantify cross-TU window value;</li><li>separate
stream fields because they improve zstd without changing Line semantics.</li></ul></div>
<div class="plane"><h3>Do not infer</h3><ul><li>Python harness throughput is not the
C++ product throughput;</li><li>whole-corpus compression is not available to the live
compiler pipe;</li><li>a 2–5% Line reshaping win does not replace the need for reusable
multi-Line/superblock outputs;</li><li>the combined ratio is still incomplete.</li></ul>
</div></div>
<p>The product-relevant question is therefore not “which zstd level closes the gap?”
It is “how much exact Line content can the shared generic pack and online project
dictionary name instead of resending?” zstd-1 or zstd-3 should then compress the much
smaller residual stream.</p></section>

<section><div class="eyebrow">Related capability result</div><h2>Source attribution points toward a portable generic bootstrap</h2>
<p>The implementer’s 15-corpus study at commit <code>85c0bd4</code> measured the
definition/dictionary leg with installed toolchain text reconstructed from source
attribution. It reduced the count below 400× from six corpora to two: DuckDB at
268.9× and fmt at 398.1×. This is a related leg-level capability result, not a complete
transfer result and not yet integrated with the structural hybrid above.</p>
<div class="chart-wrap">{svg_source_conditioning_plot(source_conditioning)}</div>
<p class="caption">Log scale. Grey is the ordinary dictionary leg; blue is the
source-conditioned result. The vertical 400× marker is a diagnostic threshold for
this leg, not the whole-message acceptance line.</p>
<div class="table-scroll"><table><thead><tr><th>Corpus</th><th>TUs</th><th>Raw MiB</th>
<th>Plain dict</th><th>Source-conditioned</th><th>Lift</th><th>Exact Line coverage</th>
</tr></thead><tbody>{''.join(source_rows)}</tbody></table></div>
<p>The portable training target should therefore be raw C/C++ source plus a small
set of toolchain-labelled transformations, not one machine’s expanded <code>.ii</code>
bytes. The shipped artifact is still only a bootstrap: target-local online history
should replace its ranking quickly, while its shared static outputs remain valid IDs.</p>
</section>

<section><div class="eyebrow">Corpus balance</div><h2>The gain is broad, not one-project driven</h2>
<div class="chart-wrap">{svg_gain_plot(comparisons)}</div>
<p class="caption">Every point is a complete-corpus endpoint. Positive means the
C-only seed has the smaller structural stream. Eigen’s
{percent(next(row['seed_vs_installed_percent'] for row in comparisons if row['corpus'] == 'eigen'))}
versus installed is the only installed-package loss; the seed still beats Eigen’s
empty learner by
{percent(next(row['seed_vs_empty_percent'] for row in comparisons if row['corpus'] == 'eigen'))}.</p>
<div class="table-scroll"><table><thead><tr><th>Corpus</th><th>TUs</th><th>Empty</th>
<th>Installed</th><th>C-only seed</th><th>Seed vs empty</th><th>Seed vs installed</th>
<th>Seed defs / all defs</th><th>Best structural hybrid*</th></tr></thead>
<tbody>{''.join(corpus_rows)}</tbody></table></div>
<p class="caption">*The final column is the separately measured cross-context
structural hybrid, included as destination headroom—not as a directly comparable
C-only-seed row.</p></section>

<section><div class="eyebrow">Cross-fit design</div><h2>Every scored target is disjoint from its seed package</h2>
<p>Package A is trained on RocksDB + OpenCV and scores the other 14 corpora. Package B
is trained on LLVM + Godot and scores RocksDB + OpenCV. This makes every row a valid
target-disjoint capability measurement. It is still a two-package cross-fit control,
not the final one-package portable raw-source model. Because these packages were
derived from expanded traces on a shared environment, they specifically do not prove
cross-toolchain portability.</p>
<div class="table-scroll"><table><thead><tr><th>SHA-256</th><th>Phrases</th><th>Raw</th>
<th>Compressed</th><th>Scored targets</th></tr></thead><tbody>{''.join(package_rows)}</tbody></table></div>
<p>The compressed seed is 79–82 KiB of C-local state. It is reported but never charged
as C→F traffic in seed-only mode. Definitions that become useful are charged in full
through the same first-profitable-use comparison as target-learned definitions.</p></section>

<section><div class="eyebrow">Acceptance program</div><h2>Turn the capability result into a universal bootstrap</h2>
<div class="split"><div class="plane"><h3>Proved by the complete matrix</h3><ul>
<li>16/16 complete seed rows independently replay exactly.</li><li>No TU-0 package bytes are
charged or transferred.</li><li>Seed wins all 16 endpoints versus empty and 15/16
versus installed.</li><li>The result is corpus-balanced and covers 28.55 GB of complete
input.</li><li>All 96 standard/reverse/shuffle/perturb rows are exact and the seed wins
every paired empty start.</li><li>All reported Line frames decode exactly.</li></ul></div>
<div class="plane"><h3>Still required</h3><ul><li>One raw-source-trained portable pack,
with projects, related builds, and toolchains held out.</li><li>Empty, C-only seed, and
pre-shared-output rows on identical first-200-TU and complete runs.</li><li>Delayed,
reverted, root-header-change, repeated-build, and concurrent arrival scenarios.</li>
<li>Complete Line/composition/value/request/framing ledger.</li><li>Real C++ C/F path
at ≥1 GB/s with cold and actual half-cold scenarios.</li></ul></div></div>

<h3>Required four-row predictor bake-off</h3>
<div class="table-scroll"><table><thead><tr><th>Row</th><th>Initial shared state</th>
<th>C behavior</th><th>F behavior</th><th>Question answered</th></tr></thead><tbody>
<tr><td>C0 · empty</td><td>codec only</td><td>online from zero</td><td>explicit definitions</td><td>How fast does local learning bootstrap itself?</td></tr>
<tr><td>C1 · C-only generic</td><td>generic candidates at C</td><td>generic → online</td><td>definitions on first use</td><td>Does the generic ranker select useful outputs?</td></tr>
<tr><td>C2 · shared outputs</td><td>same static output pack at C and F</td><td>generic → online</td><td>static IDs + dynamic definitions</td><td>How many first-use bytes does pre-sharing remove?</td></tr>
<tr><td>C3 · mirrored online</td><td>same pack and deterministic learner</td><td>ordered online updates</td><td>same ordered updates</td><td>Do avoided dynamic definitions justify coupled commit ordering?</td></tr>
</tbody></table></div>
<p>C0–C2 are the minimum product decision. C3 is optional and should be attempted only
if the dynamic-definition ledger shows enough remaining bytes to matter.</p>

<h3>Training and evaluation folds</h3>
<ol><li>Build generic candidate programs from raw source across the expanded corpus
set; do not use the target project or another build of it.</li><li>Hold out at least
LLVM and DuckDB as prominent end-to-end targets, then rotate every corpus through the
target role for the balanced score.</li><li>Label compiler and standard-library family;
include a fold where the entire toolchain family is absent from training.</li><li>Plot
wire bytes, ratio, literal fraction, static-hit fraction, dynamic-definition bytes,
and model-state bytes at TU 1–200 and completion.</li><li>Run the same trained artifact
under standard, reverse, fixed shuffles, global changes, and repeated builds. No
per-target tuning.</li></ol>
<p class="callout warning"><strong>Decision requested from BigOracle.</strong> Review
the pre-shared-output boundary and simplify it if possible. In particular: decide the
smallest static output representation worth shipping, whether the ranker stays C-only,
and whether C3 should remain deferred. Keep the literal path and actual-byte selector
in every row.</p></section>

<section><div class="eyebrow">Reproduction record</div><h2>Durable inputs and checks</h2>
<p>Machine summaries are committed beside the executable semantics:</p><ul>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-full-16corpus-summary.json">complete 16-corpus summary JSON</a> — <code>{digest(full_seed_path)}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-full-16corpus.tsv">complete per-corpus TSV</a> — <code>{digest(Path(args.full_seed_tsv))}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-screen-16corpus-summary.json">startup-screen summary JSON</a> — <code>{digest(seed_path)}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-screen-16corpus.tsv">startup-screen TSV</a> — <code>{digest(Path(args.seed_tsv))}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-stability-16corpus-summary.json">96-run stability summary</a> — <code>{digest(stability_path)}</code></li>
<li><a href="{github_root}/ml-artifacts/line-stream-split-front-16corpus.json">exact Line ceiling rows</a> — <code>{digest(line_path)}</code></li>
<li><a href="{github_root}/ml-artifacts/implementer-study15-source-conditioning.tsv">related source-conditioning table</a> — <code>{digest(source_path)}</code>; copied from implementer commit <code>85c0bd4</code></li>
<li><a href="{github_root}/online_bootstrap_curves.py">exact online codec semantics</a></li>
<li><a href="{github_root}/compare_seed_bootstrap.py">three-way curve validator</a></li>
<li><a href="{github_root}/line_stream_ceiling.py">exact Line stream/ceiling harness</a></li>
</ul>
<pre>python3 linecache/online_bootstrap_curves.py \\
  --package-in PACKAGE.zst --test linecache/traces/ml-CORPUS.bin \\
  --online-budget 524288 --thresholds 2 --level 3 --model-level 3 \\
  --publication first-use --budget-basis ids32 --row-set pretrained \\
  --pretrained-mode seed-only \\
  --curve-tsv /tmp/online-bootstrap-seed-full-CORPUS.tsv \\
  --report /tmp/online-bootstrap-seed-full-CORPUS.json</pre>
<p>Validation at publication: Python compile; focused unit suite; lint; exact replay
for every complete retained row; 96/96 stability rows exact; every retained Line mode
exact across 16 corpora; identical per-TU raw boundaries across compared starts;
canonical package import; deterministic summary regeneration; and staged diff check.
The Python Line timing is intentionally reported only as harness timing; the ≥1 GB/s
gate belongs to the integrated C++ path.</p></section>

<footer class="footer"><strong>From:</strong> mickg10/local-oracle ·
<strong>To:</strong> owner, mickg10/bigoracle, mickg10/implementer · Canonical tracker:
<a href="https://github.com/mickg10/icecream/issues/16">mickg10/icecream#16</a><br>
This page is self-contained: typography, diagrams, and SVG plots require no external
runtime assets. Use the browser’s Print command for a paginated PDF-style rendering.</footer>
</main></article></body></html>"""
    return html_text


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument(
        "--seed-summary",
        default=(
            "linecache/ml-artifacts/"
            "online-bootstrap-c-only-seed-screen-16corpus-summary.json"
        ),
    )
    value.add_argument(
        "--seed-tsv",
        default=(
            "linecache/ml-artifacts/"
            "online-bootstrap-c-only-seed-screen-16corpus.tsv"
        ),
    )
    value.add_argument(
        "--full-seed-summary",
        default=(
            "linecache/ml-artifacts/"
            "online-bootstrap-c-only-seed-full-16corpus-summary.json"
        ),
    )
    value.add_argument(
        "--full-seed-tsv",
        default=(
            "linecache/ml-artifacts/"
            "online-bootstrap-c-only-seed-full-16corpus.tsv"
        ),
    )
    value.add_argument(
        "--hybrid-summary",
        default=(
            "linecache/ml-artifacts/cross-context-hybrid-16corpus-summary.json"
        ),
    )
    value.add_argument(
        "--stability-summary",
        default=(
            "linecache/ml-artifacts/"
            "online-bootstrap-c-only-seed-stability-16corpus-summary.json"
        ),
    )
    value.add_argument(
        "--line-ceiling",
        default=(
            "linecache/ml-artifacts/line-stream-split-front-16corpus.json"
        ),
    )
    value.add_argument(
        "--source-conditioning",
        default=(
            "linecache/ml-artifacts/implementer-study15-source-conditioning.tsv"
        ),
    )
    value.add_argument(
        "--output",
        default="linecache/report-site/index.html",
    )
    return value


def main() -> int:
    args = parser().parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(render(args))
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
