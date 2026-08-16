#!/usr/bin/env python3
"""Render the issue #16 codec bake-off evidence as one standalone HTML page."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
from pathlib import Path
from typing import Sequence


COLORS = {
    "empty": "#617083",
    "installed": "#d18b2c",
    "seed": "#0e8a74",
    "ink": "#17212b",
    "grid": "#d9e0e7",
    "negative": "#b34343",
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
    seed = json.loads(seed_path.read_text())
    full_seed = json.loads(full_seed_path.read_text())
    hybrid = json.loads(hybrid_path.read_text())
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
 content="width=device-width,initial-scale=1"><title>Issue #16 · Superblock Codec Bake-off</title>
<style>{styles}</style></head><body><article class="page">
<header class="hero"><div class="kicker">Icecream protocol 50 · issue #16 · evidence snapshot</div>
<h1>Superblock codec bake-off</h1>
<p>A corpus-balanced, exact-replay report on cold-start structural coding, online
learning, and the smallest useful role for pretraining. The C-only seed removes the
upfront package transfer while preserving ordinary decoder-visible definitions.</p>
<div class="hero-meta"><span>16 corpora · {comma(endpoint['raw_bytes'])} bytes in complete runs</span>
<span>Generated 2026-08-15</span><span>Branch: local-oracle/issue16-c-only-seed</span>
<span class="status">All 16 complete seed rows exact</span></div></header><main>

<section><div class="eyebrow">Executive result</div><h2>Pretraining works best as a C-only bootstrapper</h2>
<p class="lead">Across every translation unit of all 16 complete corpora, the C-only
seed improves the equal-corpus structural ratio from
<strong>{ratio(endpoint['empty_equal_corpus_ratio'])}</strong> for empty learning and
<strong>{ratio(endpoint['installed_equal_corpus_ratio'])}</strong> for a fully installed
package to <strong>{ratio(endpoint['seed_equal_corpus_ratio'])}</strong>. It sends no
package at TU 0 and publishes only definitions that already repay themselves in the
TU carrying them.</p>
<div class="metrics">
{metric_card('C-only seed', ratio(endpoint['seed_equal_corpus_ratio']), 'Equal-corpus structural ratio at complete-corpus endpoints')}
{metric_card('Gain vs empty', percent(endpoint['seed_vs_empty_equal_percent']), 'Strictly smaller on all 16 corpus endpoints')}
{metric_card('Gain vs installed', percent(endpoint['seed_vs_installed_equal_percent']), 'Smaller on 15/16; Eigen favors installed at completion', 'warn')}
{metric_card('Exact replay', '16 / 16', 'Independent reconstruction of every Region sequence', 'neutral')}
</div>
<p class="small">The complete rows cover {comma(endpoint['raw_bytes'])} raw bytes and
send {comma(endpoint['seed_wire_bytes'])} structural bytes: a byte-weighted
{ratio(endpoint['seed_byte_weighted_ratio'])}. C-side logical learner state at the
complete endpoints ranges from
{comma(endpoint['minimum_final_logical_state_bytes'])} to
{comma(endpoint['maximum_final_logical_state_bytes'])} bytes (median
{comma(int(endpoint['median_final_logical_state_bytes']))}). This is deterministic
key-and-counter accounting, not Python process RSS or a product allocation estimate.</p>
<div class="callout warning"><strong>Scope boundary.</strong> These are exact
Region-program structural bytes, not complete <code>.ii</code> transfer. Line text,
Region-to-Line composition, typed values, literal residuals, missing-object exchanges,
and production framing are not in this startup ratio. No total cold-400 claim is made.</div>
</section>

<section><div class="eyebrow">Whole objective ledger</div><h2>The structural headroom is real; cold total remains open</h2>
<p>The best full-run structural hybrid reaches {ratio(hybrid['equal_corpus_ratio'])}
equal-corpus and {ratio(hybrid['aggregate_ratio'])} byte-weighted. Once the separately
measured first-use Line-text leg is added, the optimistic incomplete cold result falls
to 183.49×. Half-cold has provisional headroom at 262.80×, but still needs the real
cache scenario and every remaining block.</p>
<div class="goal-row"><strong>Complete cold</strong><div class="goal-track"><div
 class="goal-fill warning" style="width:{183.49/400*100:.2f}%"></div></div>
<div class="goal-value">183.49 / 400×</div></div>
<div class="goal-row"><strong>Complete half-cold</strong><div class="goal-track"><div
 class="goal-fill" style="width:100%"></div></div><div class="goal-value">262.80 / 200×*</div></div>
<div class="goal-row"><strong>Structural installed</strong><div class="goal-track"><div
 class="goal-fill" style="width:100%"></div></div><div class="goal-value">462.92×</div></div>
<p class="small">*Provisional and incomplete: passing the numerical threshold here is
not the same as passing the complete half-cold acceptance scenario.</p></section>

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

<section><div class="eyebrow">Protocol algorithm</div><h2>One existing definition path, no F-side predictor</h2>
<div class="flow"><div class="flow-box"><strong>1 · C candidate state</strong>Start with
the target-disjoint seed plus bounded chronological learning. This state is never
automatically transferred.</div><div class="arrow">→</div><div class="flow-box"><strong>2 · Actual-byte choice</strong>
Encode empty baseline and seed/online candidate from the same pre-TU state. Include
all definition, payload, length, and selector bytes.</div><div class="arrow">→</div>
<div class="flow-box"><strong>3 · Ordinary wire blocks</strong>If and only if the
candidate is smaller, send its exact missing phrase definitions followed by its
payload. Otherwise send the baseline.</div><div class="arrow">→</div><div class="flow-box">
<strong>4 · F exact expansion</strong>Install received immutable definitions, decode
the selected payload, reproduce the complete Region sequence, then advance state.</div></div>
<div class="split"><div class="plane"><h3>C retains</h3><ul><li>bounded candidate
learner;</li><li>4,476- or 4,557-phrase seed vocabulary;</li><li>append-only installed
definition IDs shared with F;</li><li>dense Region IDs from completed TUs.</li></ul></div>
<div class="plane"><h3>F retains</h3><ul><li>only definitions actually received;</li>
<li>the same append-only installed IDs;</li><li>dense Region IDs independently rebuilt
from exact output;</li><li>no seed package and no duplicate learner.</li></ul></div></div>
<h3>Definition encoding</h3><p>A single versioned definition batch chooses per phrase:
(a) delta-coded dense Region references when every Region has already been observed,
or (b) the exact phrase key when a seed phrase contains first-seen Regions. The
receiver accepts known references, exact canonical keys, complete frames, and unique
definitions. This is a refinement of the existing definition block, not a
new message family.</p>
<ol><li>Encode TU <em>t</em> from state through <em>t−1</em>.</li><li>Compare complete
current-TU candidates using actual zstd-3 bytes.</li><li>Commit and install only the
selected candidate’s definitions.</li><li>Decode independently and byte-compare the
complete Region sequence.</li><li>Only after completion expose TU <em>t</em> to learning
for future TUs.</li></ol></section>

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
not the final one-package portable raw-source model.</p>
<div class="table-scroll"><table><thead><tr><th>SHA-256</th><th>Phrases</th><th>Raw</th>
<th>Compressed</th><th>Scored targets</th></tr></thead><tbody>{''.join(package_rows)}</tbody></table></div>
<p>The compressed seed is 79–82 KiB of C-local state. It is reported but never charged
as C→F traffic in seed-only mode. Definitions that become useful are charged in full
through the same first-profitable-use comparison as target-learned definitions.</p></section>

<section><div class="eyebrow">Acceptance and next experiment</div><h2>What this proves—and what must happen next</h2>
<div class="split"><div class="plane"><h3>Proved by the complete matrix</h3><ul>
<li>16/16 complete seed rows independently replay exactly.</li><li>No TU-0 package bytes are
charged or transferred.</li><li>Seed wins all 16 endpoints versus empty and 15/16
versus installed.</li><li>The result is corpus-balanced and covers 28.55 GB of complete
input.</li><li>Mixed definitions and unseen-Region publication have focused
tests.</li></ul></div><div class="plane"><h3>Still required</h3><ul>
<li>Reverse, multiple fixed shuffles, scheduler-like reorder, shared-Region change,
and delayed/reverted
change matrices.</li><li>One portable raw-source seed protocol across toolchains.</li>
<li>Complete Line/value/residual/request/framing ledger.</li><li>Real C++ C/F path at
≥1 GB/s with cold and actual half-cold scenarios.</li></ul></div></div>
<p class="callout warning"><strong>Decision requested from BigOracle.</strong> Keep the
C-only seed as initialization of the existing C candidate store unless the expanded
or stability matrix finds a broad loss. Do not add an independent F predictor or a new
message kind to rescue a single corpus.</p></section>

<section><div class="eyebrow">Reproduction record</div><h2>Durable inputs and checks</h2>
<p>Machine summaries are committed beside the executable semantics:</p><ul>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-full-16corpus-summary.json">complete 16-corpus summary JSON</a> — <code>{digest(full_seed_path)}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-full-16corpus.tsv">complete per-corpus TSV</a> — <code>{digest(Path(args.full_seed_tsv))}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-screen-16corpus-summary.json">startup-screen summary JSON</a> — <code>{digest(seed_path)}</code></li>
<li><a href="{github_root}/ml-artifacts/online-bootstrap-c-only-seed-screen-16corpus.tsv">startup-screen TSV</a> — <code>{digest(Path(args.seed_tsv))}</code></li>
<li><a href="{github_root}/online_bootstrap_curves.py">exact online codec semantics</a></li>
<li><a href="{github_root}/compare_seed_bootstrap.py">three-way curve validator</a></li>
</ul>
<pre>python3 linecache/online_bootstrap_curves.py \\
  --package-in PACKAGE.zst --test linecache/traces/ml-CORPUS.bin \\
  --online-budget 524288 --thresholds 2 --level 3 --model-level 3 \\
  --publication first-use --budget-basis ids32 --row-set pretrained \\
  --pretrained-mode seed-only \\
  --curve-tsv /tmp/online-bootstrap-seed-full-CORPUS.tsv \\
  --report /tmp/online-bootstrap-seed-full-CORPUS.json</pre>
<p>Validation at publication: Python compile; focused unit suite; lint; exact replay
for every complete retained row; identical per-TU raw boundaries across all three starts;
canonical package import; deterministic summary regeneration; and staged diff check.</p></section>

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
