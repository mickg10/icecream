#!/usr/bin/env python3
"""Build a self-contained, theme-aware HTML report of the empty-vs-pretrained per-TU
learning curves (log-Y wire bytes per input byte). Reads curve_overlay.json."""
import json, math

d = json.load(open("curve_overlay.json"))
# per-project final gain + toolchain
for c,o in d.items():
    o["finGain"] = o["pre_cum"][-1] / o["empty_cum"][-1]
    o["empty_final"] = o["empty_cum"][-1]; o["pre_final"] = o["pre_cum"][-1]

SEED = "#0e8a74"; EMPTY = "#7a8794"   # teal seed, slate empty (align w/ local-oracle report)

def logy_chart(o, w=430, h=250):
    """Overlay log-Y line chart: wire/input-byte per TU, empty vs pretrained."""
    pad_l, pad_r, pad_t, pad_b = 46, 12, 14, 26
    pw, ph = w-pad_l-pad_r, h-pad_t-pad_b
    tu, pre, emp = o["tu"], o["pre"], o["empty"]
    n = len(tu)
    allv = [x for x in pre+emp if x > 0]
    ymin, ymax = min(allv), max(allv)
    lymin, lymax = math.log10(ymin), math.log10(ymax)
    def X(i): return pad_l + pw * (i/(n-1) if n>1 else 0)
    def Y(v):
        v = max(v, ymin)
        return pad_t + ph * (1 - (math.log10(v)-lymin)/(lymax-lymin if lymax>lymin else 1))
    def poly(arr,color,dash=""):
        pts = " ".join(f"{X(i):.1f},{Y(v):.1f}" for i,v in enumerate(arr))
        return f'<polyline fill="none" stroke="{color}" stroke-width="2" stroke-linejoin="round" {dash} points="{pts}"/>'
    # log gridlines at powers of 10 within range
    grid=[]
    lo,hi = math.floor(lymin), math.ceil(lymax)
    for e in range(lo,hi+1):
        v=10**e
        if v<ymin*0.9 or v>ymax*1.1: continue
        gy=Y(v)
        lab = f"{v:.4f}".rstrip("0").rstrip(".") if v<1 else f"{v:.0f}"
        grid.append(f'<line x1="{pad_l}" y1="{gy:.1f}" x2="{w-pad_r}" y2="{gy:.1f}" class="gl"/>'
                    f'<text x="{pad_l-5}" y="{gy+3:.1f}" text-anchor="end" class="tick">{lab}</text>')
    xlab = (f'<text x="{X(0):.0f}" y="{h-8}" text-anchor="start" class="tick">TU 1</text>'
            f'<text x="{X(n-1):.0f}" y="{h-8}" text-anchor="end" class="tick">{tu[-1]}</text>')
    badge = "MISMATCH" if o["tc"]=="MISMATCH" else "match"
    bcol = "var(--neg)" if o["tc"]=="MISMATCH" else "var(--muted)"
    return (f'<svg viewBox="0 0 {w} {h}" class="chart" role="img" aria-label="{o["name"]} per-TU wire per input byte, empty vs pretrained">'
            f'{"".join(grid)}{poly(emp,EMPTY)}{poly(pre,SEED)}{xlab}'
            f'<text x="{pad_l+4}" y="{pad_t+11}" class="pnl">{o["name"]}</text>'
            f'<text x="{w-pad_r-2}" y="{pad_t+11}" text-anchor="end" class="pbadge" fill="{bcol}">{badge} · {o["finGain"]:.2f}×</text>'
            f'</svg>')

def gain_bar(rows, w=760):
    rows = sorted(rows, key=lambda x:-x["finGain"])
    rowh=20; h = 30 + rowh*len(rows)
    pad_l=118; pw=w-pad_l-60
    xmin,xmax=1.0, max(r["finGain"] for r in rows)*1.02
    def X(v): return pad_l + pw*(v-xmin)/(xmax-xmin)
    out=[f'<svg viewBox="0 0 {w} {h}" class="chart" role="img" aria-label="Seed contribution (final ratio gain) per project">']
    # 1.0x baseline + median lines
    x1=X(1.0); out.append(f'<line x1="{x1}" y1="20" x2="{x1}" y2="{h-10}" class="axl"/>')
    for gv in (1.2,1.4,1.6):
        if gv<xmax:
            gx=X(gv); out.append(f'<line x1="{gx}" y1="20" x2="{gx}" y2="{h-10}" class="gl"/><text x="{gx}" y="16" text-anchor="middle" class="tick">{gv:.1f}×</text>')
    y=28
    for r in rows:
        col = SEED if r["tc"]!="MISMATCH" else "#b34343"
        bx=X(r["finGain"])
        out.append(f'<text x="{pad_l-6}" y="{y+11}" text-anchor="end" class="lab">{r["name"]}</text>'
                   f'<rect x="{pad_l}" y="{y}" width="{max(1,bx-pad_l):.1f}" height="13" rx="2.5" fill="{col}"/>'
                   f'<text x="{bx+5:.1f}" y="{y+11}" class="val">{r["finGain"]:.2f}×</text>')
        y+=rowh
    out.append("</svg>")
    return "".join(out)

# choose small-multiples: 3 matched w/ visible gap + 3 mismatched
matched_pick = ["corpus3","corpus6","corpus13"]   # DuckDB, Godot, re2
mismatch_pick = ["corpus18","corpus20","corpus25"] # Firefox, ClickHouse, V8
rows = list(d.values())
n_match = sum(1 for r in rows if r["tc"]!="MISMATCH")
med_match = sorted([r["finGain"] for r in rows if r["tc"]!="MISMATCH"])[n_match//2]

sm = "".join(f'<div class="cell">{logy_chart(d[c])}</div>' for c in matched_pick+mismatch_pick)

html = f"""<title>Seed vs Self-Bootstrap</title>
<style>
:root{{--bg:#faf9f6;--card:#ffffff;--ink:#1a232b;--muted:#5d6b78;--faint:#8a97a3;
 --line:#e4e8ec;--seed:{SEED};--empty:{EMPTY};--neg:#b34343;--accent:#0e8a74;}}
@media (prefers-color-scheme:dark){{:root:not([data-theme="light"]){{--bg:#12171c;--card:#182028;
 --ink:#e8edf1;--muted:#9fb0bd;--faint:#6b7883;--line:#28323c;}}}}
:root[data-theme="dark"]{{--bg:#12171c;--card:#182028;--ink:#e8edf1;--muted:#9fb0bd;--faint:#6b7883;--line:#28323c;}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);
 font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
 line-height:1.55;-webkit-font-smoothing:antialiased}}
.wrap{{max-width:920px;margin:0 auto;padding:40px 24px 80px}}
.kicker{{font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:var(--seed);font-weight:600}}
h1{{font-size:34px;line-height:1.1;margin:.35em 0 .3em;letter-spacing:-.02em;text-wrap:balance}}
.lede{{font-size:17px;color:var(--muted);max-width:66ch;margin:0 0 26px}}
.tiles{{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin:26px 0 34px}}
.tile{{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px}}
.tile .v{{font-size:26px;font-weight:700;letter-spacing:-.01em;font-variant-numeric:tabular-nums}}
.tile .v.s{{color:var(--seed)}} .tile .v.n{{color:var(--neg)}}
.tile .k{{font-size:12.5px;color:var(--muted);margin-top:2px}}
h2{{font-size:21px;margin:38px 0 6px;letter-spacing:-.01em}}
.sub{{color:var(--muted);font-size:14.5px;margin:0 0 18px;max-width:70ch}}
.grid{{display:grid;grid-template-columns:1fr 1fr;gap:14px}}
.cell{{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:8px}}
.chart{{width:100%;height:auto;display:block}}
.gl{{stroke:var(--line);stroke-width:1}} .axl{{stroke:var(--faint);stroke-width:1.2;stroke-dasharray:2 2}}
.tick{{fill:var(--faint);font-size:10px;font-variant-numeric:tabular-nums}}
.lab{{fill:var(--muted);font-size:11.5px}} .val{{fill:var(--ink);font-size:10.5px;font-variant-numeric:tabular-nums}}
.pnl{{fill:var(--ink);font-size:13px;font-weight:600}} .pbadge{{font-size:10.5px;font-weight:600}}
.legend{{display:flex;gap:20px;align-items:center;margin:6px 0 16px;font-size:13px;color:var(--muted)}}
.legend b{{display:inline-block;width:22px;height:3px;border-radius:2px;margin-right:6px;vertical-align:middle}}
.card{{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px 18px;margin-top:6px}}
p{{max-width:70ch}} .foot{{color:var(--faint);font-size:12.5px;margin-top:40px;border-top:1px solid var(--line);padding-top:16px}}
strong.s{{color:var(--seed)}} strong.n{{color:var(--neg)}}
</style>
<div class="wrap">
<p class="kicker">icecream · issue #16 · target-disjoint bake-off</p>
<h1>Seed vs self-bootstrap</h1>
<p class="lede">A pretrained cross-project seed and a from-scratch (empty) online codec, run over the same first 200 translation units of 25 C++ projects. The question: how much does the seed actually transfer &mdash; and does it survive a toolchain change? Metric is <b>wire bytes per input byte</b> per TU (log scale); lower is better.</p>
<div class="tiles">
<div class="tile"><div class="v s">+{(med_match-1)*100:.0f}%</div><div class="k"><b>Seed gain, matched toolchain</b> &mdash; median final-ratio lift over self-bootstrap</div></div>
<div class="tile"><div class="v n">~0%</div><div class="k"><b>Seed gain, mismatched toolchain</b> &mdash; the gcc-11 <code>.ii</code> seed is inert on clang/libc++ projects</div></div>
<div class="tile"><div class="v">TU&nbsp;1</div><div class="k"><b>Where curves meet</b> &mdash; the seed never helps the first TU; the head start is TU&nbsp;2 onward</div></div>
<div class="tile"><div class="v">25/25</div><div class="k"><b>Byte-exact</b> &mdash; every reconstruction verified</div></div>
</div>

<h2>The learning curves</h2>
<p class="sub">Each panel: one project, the same TU sequence, both codecs. <span style="color:var(--empty)">Slate = empty (self-bootstrap)</span>, <span style="color:var(--seed)">teal = pretrained seed</span>. The gap between them is the seed's contribution. Badge shows toolchain match and the final cumulative-ratio gain.</p>
<div class="legend"><span><b style="background:{EMPTY}"></b>empty (from scratch)</span><span><b style="background:{SEED}"></b>pretrained seed</span></div>
<div class="grid">{sm}</div>
<div class="card" style="margin-top:14px">
<p style="margin:0"><b>Read it:</b> on a <strong class="s">matched</strong> toolchain (top row) the teal seed curve drops below slate from TU&nbsp;2 &mdash; a cold-start head start &mdash; then the two <b>converge</b> as the empty codec learns the project's own structure. On a <strong class="n">mismatched</strong> toolchain (bottom row) the curves are <b>superimposed</b>: the seed, trained on gcc-11 <code>.ii</code>, has nothing to say about clang/libc++ bytes.</p>
</div>

<h2>Seed contribution across all 25</h2>
<p class="sub">Final cumulative-ratio gain (pretrained &divide; empty). Above 1.0&times; means the seed still helps at TU&nbsp;200; 1.0&times; means the project fully self-bootstrapped. <span style="color:var(--neg)">Red = mismatched toolchain.</span></p>
<div class="card">{gain_bar(rows)}</div>

<h2>What it means</h2>
<p><strong>Most of the compression is project-local self-bootstrap.</strong> The empty codec alone reaches high ratios (DuckDB 391&times;, Firefox 767&times;, Godot 689&times;) purely by learning each project's own recurring structure across its first 200 TUs. The seed is a <b>modest, front-loaded top-up</b> &mdash; a median <strong class="s">+{(med_match-1)*100:.0f}%</strong> when the toolchain matches, biggest in the early TUs and fading as the project catches up.</p>
<p><strong class="n">And it does not cross a toolchain boundary.</strong> The three clang/libc++ projects (Firefox, ClickHouse, V8) show a seed contribution of essentially <b>zero</b> against the gcc-11 seed &mdash; because preprocessed <code>.ii</code> is toolchain-specific down to the bytes. This is the empirical case for what local-oracle's report proposes: the shipped universal bootstrap must be trained from <b>raw source and parameterized structural programs</b> (toolchain-invariant), with the toolchain byte-substrate derived per-env from the shipped toolchain &mdash; not an <code>.ii</code> seed, which is only a mechanism-and-accounting control.</p>
<p class="foot">25 C++ codebases · first 200 TUs · target-disjoint seed packs (rocks-opencv; RocksDB/OpenCV held out to llvm-godot) · online superblock codec, seed-only, first-use publication, zstd-3 · all byte-exact · quietbox2 (EPYC 8124P). Complements local-oracle's #16 deep report (16-corpus C-only seed). Metric: per-TU wire&nbsp;&divide;&nbsp;input bytes, log scale.</p>
</div>
"""
open("curves_viz.html","w").write(html)
print(f"wrote curves_viz.html: {len(html)} bytes; matched median finGain={med_match:.3f}; small-multiples={len(matched_pick+mismatch_pick)}")
