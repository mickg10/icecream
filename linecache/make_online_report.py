#!/usr/bin/env python3
"""
make_online_report.py — build a self-contained HTML learning-curve report from the
superblock-online-bench outputs (per REPORT-FORMAT-ASK.md / CURVE-DEFINITION.md).

One-command regen:
  python3 make_online_report.py --prefix curve-llvm --log curve-llvm.log \
      --corpus LLVM --out superblock-online-report.html

Inputs (produced by: superblock-online-bench --curves <prefix> --sweep):
  <prefix>-pertu.tsv    per-TU prequential points
  <prefix>-perloop.tsv  per-loop endpoints
  <log>                 stderr/stdout log (F-sweep + threshold-sweep + header stats)

Every plotted point comes from those files; the report embeds the per-loop table and links
the raw TSVs so alternative plots can be regenerated. Full-charged-wire is the primary metric.
"""
import argparse, csv, html, math, re, sys, subprocess, datetime

# ---- validated dataviz palette (reference instance) ----
SER = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]      # light: blue, orange, aqua, yellow
SER_D = ["#3987e5", "#d95926", "#199e70", "#c98500"]    # dark

def read_tsv(path):
    with open(path) as f:
        return list(csv.DictReader(f, delimiter="\t"))

def num(x):
    try: return float(x)
    except: return 0.0

# ---------------- SVG line chart (log or linear y) ----------------
def line_chart(series, xlabel, ylabel, width=920, height=340, ylog=False, xmax=None,
               vlines=None, yfmt=lambda v: f"{v:g}", title=""):
    """series: list of dicts {name, color_idx, pts:[(x,y)], markers:bool, faint:bool}"""
    pad_l, pad_r, pad_t, pad_b = 64, 130, 28, 40
    xs = [x for s in series for (x, y) in s["pts"]]
    ys = [y for s in series for (x, y) in s["pts"] if (y > 0 or not ylog)]
    if not xs or not ys: return f'<p>(no data for {html.escape(title)})</p>'
    xmn, xmx = 0, (xmax if xmax else max(xs))
    if ylog:
        ymn = min(y for y in ys if y > 0); ymx = max(ys)
        ymn = 10 ** math.floor(math.log10(ymn)); ymx = 10 ** math.ceil(math.log10(ymx))
        def ty(v):
            v = max(v, ymn)
            return pad_t + (height - pad_t - pad_b) * (1 - (math.log10(v) - math.log10(ymn)) / (math.log10(ymx) - math.log10(ymn)))
    else:
        ymn = 0; ymx = max(ys) * 1.08
        def ty(v): return pad_t + (height - pad_t - pad_b) * (1 - (v - ymn) / (ymx - ymn or 1))
    def tx(v): return pad_l + (width - pad_l - pad_r) * ((v - xmn) / (xmx - xmn or 1))
    out = [f'<svg viewBox="0 0 {width} {height}" class="chart" role="img" aria-label="{html.escape(title)}">']
    # y gridlines + ticks
    if ylog:
        d = int(math.log10(ymn)); ticks = []
        while 10 ** d <= ymx + 1e-12:
            ticks.append(10 ** d); d += 1
    else:
        ticks = [ymn + (ymx - ymn) * i / 4 for i in range(5)]
    for tk in ticks:
        y = ty(tk)
        out.append(f'<line x1="{pad_l}" y1="{y:.1f}" x2="{width-pad_r}" y2="{y:.1f}" class="grid"/>')
        out.append(f'<text x="{pad_l-8}" y="{y+3:.1f}" text-anchor="end" class="tick">{yfmt(tk)}</text>')
    # x ticks
    for i in range(6):
        xv = xmn + (xmx - xmn) * i / 5; x = tx(xv)
        out.append(f'<line x1="{x:.1f}" y1="{pad_t}" x2="{x:.1f}" y2="{height-pad_b}" class="grid"/>')
        out.append(f'<text x="{x:.1f}" y="{height-pad_b+16:.1f}" text-anchor="middle" class="tick">{int(xv)}</text>')
    # loop boundary markers
    for (vx, lab) in (vlines or []):
        if vx > xmx: continue
        x = tx(vx)
        out.append(f'<line x1="{x:.1f}" y1="{pad_t}" x2="{x:.1f}" y2="{height-pad_b}" class="vmark"/>')
        out.append(f'<text x="{x+2:.1f}" y="{pad_t+10:.1f}" class="vlab">{html.escape(lab)}</text>')
    # axis labels
    out.append(f'<text x="{(pad_l+width-pad_r)/2:.0f}" y="{height-4}" text-anchor="middle" class="axlab">{html.escape(xlabel)}</text>')
    out.append(f'<text x="14" y="{(pad_t+height-pad_b)/2:.0f}" text-anchor="middle" class="axlab" transform="rotate(-90 14 {(pad_t+height-pad_b)/2:.0f})">{html.escape(ylabel)}</text>')
    # series
    for s in series:
        ci = s["color_idx"]
        pts = [(tx(x), ty(y)) for (x, y) in s["pts"] if (y > 0 or not ylog)]
        if not pts: continue
        path = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        op = 0.30 if s.get("faint") else 1.0
        w = 1.2 if s.get("faint") else 2.0
        out.append(f'<path d="{path}" fill="none" stroke="var(--s{ci})" stroke-width="{w}" opacity="{op}" stroke-linejoin="round" stroke-linecap="round"/>')
        if s.get("markers"):
            for (x, y), (rawx, rawy) in zip(pts, [(x, y) for (x, y) in s["pts"] if (y > 0 or not ylog)]):
                out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="3.2" fill="var(--s{ci})" stroke="var(--surface-1)" stroke-width="1.5"><title>{html.escape(s["name"])}: x={rawx:g}, {yfmt(rawy)}</title></circle>')
        # direct label at right end
        lx, ly = pts[-1]
        out.append(f'<text x="{lx+6:.1f}" y="{ly+3:.1f}" class="slab" fill="var(--s{ci})">{html.escape(s["name"])}</text>')
    out.append('</svg>')
    return "".join(out)

def bars(groups, series_names, ylabel, yfmt=lambda v: f"{v:g}", width=920, height=320, title=""):
    """groups: list of (label, [values per series]). grouped bars."""
    pad_l, pad_r, pad_t, pad_b = 64, 130, 24, 40
    allv = [v for _, vs in groups for v in vs]
    if not allv: return "<p>(no data)</p>"
    ymx = max(allv) * 1.08
    def ty(v): return pad_t + (height - pad_t - pad_b) * (1 - v / (ymx or 1))
    n = len(groups); ns = len(series_names)
    gw = (width - pad_l - pad_r) / n; bw = gw * 0.8 / ns
    out = [f'<svg viewBox="0 0 {width} {height}" class="chart" role="img" aria-label="{html.escape(title)}">']
    for i in range(5):
        v = ymx * i / 4; y = ty(v)
        out.append(f'<line x1="{pad_l}" y1="{y:.1f}" x2="{width-pad_r}" y2="{y:.1f}" class="grid"/>')
        out.append(f'<text x="{pad_l-8}" y="{y+3:.1f}" text-anchor="end" class="tick">{yfmt(v)}</text>')
    for gi, (lab, vs) in enumerate(groups):
        gx = pad_l + gi * gw + gw * 0.1
        for si, v in enumerate(vs):
            x = gx + si * bw; y = ty(v); h = (height - pad_b) - y
            out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bw*0.86:.1f}" height="{max(h,0):.1f}" rx="3" fill="var(--s{si})"><title>{html.escape(series_names[si])} @ {html.escape(lab)}: {yfmt(v)}</title></rect>')
        out.append(f'<text x="{gx+gw*0.4:.1f}" y="{height-pad_b+16:.1f}" text-anchor="middle" class="tick">{html.escape(lab)}</text>')
    out.append(f'<text x="14" y="{(pad_t+height-pad_b)/2:.0f}" text-anchor="middle" class="axlab" transform="rotate(-90 14 {(pad_t+height-pad_b)/2:.0f})">{html.escape(ylabel)}</text>')
    lx = width - pad_r + 12
    for si, nm in enumerate(series_names):
        out.append(f'<rect x="{lx}" y="{pad_t+si*20}" width="12" height="12" rx="2" fill="var(--s{si})"/>')
        out.append(f'<text x="{lx+18}" y="{pad_t+si*20+10}" class="slab">{html.escape(nm)}</text>')
    out.append('</svg>')
    return "".join(out)

def moving_avg(pts, w):
    out = []; from collections import deque; q = deque(); s = 0.0
    for x, y in pts:
        q.append(y); s += y
        if len(q) > w: s -= q.popleft()
        out.append((x, s / len(q)))
    return out

def downsample(pts, n=400):
    if len(pts) <= n: return pts
    step = len(pts) // n
    return pts[::step]

def parse_log(logtext):
    d = {}
    m = re.search(r"TUs\(build\)=(\d+) rawA=([\d.]+) MiB.*distinct_lines\(A/final\)=(\d+)/(\d+).*distinct_regions\(A/final\)=(\d+)/(\d+).*promote_count=(\d+)", logtext)
    if m: d["hdr"] = m.groups()
    m = re.search(r"OFFLINE region-BPE ceiling: blocks=(\d+)\s+A-root-tokens=(\d+)\s+root-raw=([\d.]+) MiB", logtext)
    if m: d["ceiling"] = m.groups()
    m = re.search(r"block lifetime -- total=(\d+) used>=1=(\d+) used>=2=(\d+) never=(\d+).*max_depth=(\d+)", logtext)
    if m: d["life"] = m.groups()
    # F-sweep rows: "A COLD_FIRST 4 rr | root def missing framing TOTAL"
    fsweep = []
    for mm in re.finditer(r"^(A COLD_FIRST|B WARM_SAME|C HEADER_EDIT|D CHANGED_STEADY)\s+(\d+)\s+(rr|sticky)\s+\|\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)", logtext, re.M):
        fsweep.append(mm.groups())
    d["fsweep"] = fsweep
    # threshold sweep rows: "promote blocksB B_tok/reg B_root_z3"
    thr = []
    for mm in re.finditer(r"^\s*(\d+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s*$", logtext, re.M):
        thr.append(mm.groups())
    d["thr"] = thr
    return d

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--corpus", default="LLVM")
    ap.add_argument("--commit", default="")
    ap.add_argument("--cmd", default="")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    pertu = read_tsv(a.prefix + "-pertu.tsv")
    perloop = read_tsv(a.prefix + "-perloop.tsv")
    logtext = open(a.log).read() if a.log else ""
    L = parse_log(logtext)

    # loop boundaries (x at each loop end) + labels
    vlines = []
    for r in perloop:
        vlines.append((num(r["x_end"]), f'{r["phase"][0]}{r["loop"]}'))
    xmax = max(num(r["x_end"]) for r in perloop) if perloop else None
    # cold/warm/edit/revert phase boundaries only (fewer labels)
    phase_bounds = []; last = None
    acc = 0
    for r in perloop:
        if r["phase"] != last:
            phase_bounds.append((num(r["x_end"]) - num(r["TUs"]), r["phase"])); last = r["phase"]

    # per-loop series
    def loop_pts(col): return [(num(r["x_end"]), num(r[col])) for r in perloop]
    # per-TU rolling (bits/byte, tok/reg) for faint texture
    tu_bpb = [(num(r["x_obs"]), num(r["bits_per_byte"])) for r in pertu]
    tu_bpb_ma = downsample(moving_avg(tu_bpb, 128))
    tu_tr = [(num(r["x_obs"]), num(r["root_tok"]) / max(num(r["region_count"]), 1)) for r in pertu]
    tu_tr_ma = downsample(moving_avg(tu_tr, 128))

    # ---- charts ----
    # 1. effective compression (bits/input-byte), log y
    c1 = line_chart(
        [ {"name":"per-loop","color_idx":0,"pts":loop_pts("bits_per_byte"),"markers":True},
          {"name":"per-TU MA128","color_idx":2,"pts":tu_bpb_ma,"faint":True} ],
        "TUs observed (across loops)", "bits / input byte  (log, lower=better)",
        ylog=True, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: (f"{v:g}"),
        title="Effective end-to-end compression learning curve")
    # 2. predictor quality Q (linear)
    c2 = line_chart(
        [ {"name":"Q per-loop","color_idx":1,"pts":loop_pts("Q_loop"),"markers":True} ],
        "TUs observed (across loops)", "predictor quality Q = (B-O)/(B-A)",
        ylog=False, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: f"{v:.2f}",
        title="Prequential predictor-quality curve (1.0 = batch region-BPE reference)")
    # 3. root tokens / region (log)
    c3 = line_chart(
        [ {"name":"per-loop","color_idx":0,"pts":loop_pts("tok_per_reg"),"markers":True},
          {"name":"per-TU MA128","color_idx":2,"pts":tu_tr_ma,"faint":True} ],
        "TUs observed (across loops)", "root tokens / input region  (log)",
        ylog=True, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: f"{v:g}",
        title="Structure-learning curve: emitted root tokens per input region")
    # 4. published blocks growth
    c4 = line_chart(
        [ {"name":"blocks published","color_idx":3,"pts":loop_pts("blocks_pub"),"markers":True} ],
        "TUs observed (across loops)", "published immutable blocks",
        ylog=False, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: f"{int(v)}",
        title="Learner-state growth: published block count")
    # 5. F-cache multiplication (cold A def_raw, rr vs sticky), MiB
    fs = L.get("fsweep", [])
    coldA = {}
    for (pass_, F, pol, root, defr, miss, fram, tot) in fs:
        if pass_ == "A COLD_FIRST": coldA.setdefault(int(F), {})[pol] = float(defr)/1048576.0
    groups5 = [(f"{F}F", [coldA.get(F,{}).get("rr",0), coldA.get(F,{}).get("sticky",0)]) for F in sorted(coldA)]
    c5 = bars(groups5, ["round-robin","sticky/affinity"], "cold-A definition transfer (MiB)",
              yfmt=lambda v: f"{v:.0f}", title="Per-F definition multiplication (cold build)")
    # 6. threshold sweep: tok/reg vs promote
    thr = L.get("thr", [])
    c6 = line_chart(
        [ {"name":"B tok/reg","color_idx":1,"pts":[(int(p),float(tr)) for (p,b,tr,z) in thr],"markers":True} ],
        "promote threshold", "warm-B tok/region",
        ylog=False, xmax=(max(int(p) for p,_,_,_ in thr) if thr else None), yfmt=lambda v:f"{v:.3f}",
        title="Promotion-gate sweep: covering vs threshold") if thr else "<p>(no sweep)</p>"

    # ---- metadata ----
    hdr = L.get("hdr"); ceil = L.get("ceiling"); life = L.get("life")
    try: gcc = subprocess.check_output(["g++","--version"]).decode().splitlines()[0]
    except Exception: gcc = "g++ (unknown)"
    try: host = subprocess.check_output(["uname","-srm"]).decode().strip()
    except Exception: host = "linux"
    now = datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%MZ")

    # per-loop table (headline loops + all)
    def loop_table():
        cols = ["phase","loop","x_end","TUs","sum_root_tok","Q_loop","tok_per_reg","bits_per_byte","blocks_pub","new_blk_loop"]
        th = "".join(f"<th>{html.escape(c)}</th>" for c in cols)
        rows = []
        for r in perloop:
            tds = "".join(f"<td>{html.escape(r.get(c,''))}</td>" for c in cols)
            rows.append(f"<tr>{tds}</tr>")
        return f'<table class="data"><thead><tr>{th}</tr></thead><tbody>{"".join(rows)}</tbody></table>'

    def stat(label, val, sub=""):
        return f'<div class="stat"><div class="sv">{html.escape(val)}</div><div class="sl">{html.escape(label)}</div>{f"<div class=sub>{html.escape(sub)}</div>" if sub else ""}</div>'

    # headline stats: use the FIRST warm rebuild (realistic) not the identical-repeat asymptote.
    warm1 = next((r for r in perloop if r["phase"]=="warm" and r["loop"]=="1"), None)
    warm_final = next((r for r in reversed(perloop) if r["phase"]=="warm"), None)
    cold = next((r for r in perloop if r["phase"]=="cold"), None)
    stats = ""
    if hdr and warm1 and cold and warm_final:
        bpb1 = num(warm1["bits_per_byte"])
        stats = "".join([
            stat("corpus", a.corpus, f'{hdr[0]} TUs · {hdr[1]} MiB source'),
            stat("cold → 1st rebuild tok/region", f'{num(cold["tok_per_reg"]):.3f} → {num(warm1["tok_per_reg"]):.3f}', 'keeps halving per identical rebuild'),
            stat("1st-rebuild bits/input-byte", f'{bpb1:.5f}', f'≈ {1/max(bpb1/8,1e-12):.0f}× effective compression'),
            stat("wire vs cold", f'{num(cold["bits_per_byte"])/max(bpb1,1e-12):.0f}×', 'smaller after one warm rebuild'),
            stat("published blocks (final)", warm_final["blocks_pub"], f'{life[1]}/{life[0]} ever used' if life else ''),
        ])

    css = """
:root{--surface-1:#fcfcfb;--plane:#f9f9f7;--text-primary:#0b0b0b;--text-secondary:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;--border:rgba(11,11,11,.10);--s0:#2a78d6;--s1:#eb6834;--s2:#1baf7a;--s3:#eda100;}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--surface-1:#1a1a19;--plane:#0d0d0d;--text-primary:#fff;--text-secondary:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--border:rgba(255,255,255,.10);--s0:#3987e5;--s1:#d95926;--s2:#199e70;--s3:#c98500;}}
:root[data-theme=dark]{--surface-1:#1a1a19;--plane:#0d0d0d;--text-primary:#fff;--text-secondary:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--border:rgba(255,255,255,.10);--s0:#3987e5;--s1:#d95926;--s2:#199e70;--s3:#c98500;}
*{box-sizing:border-box}body{margin:0;background:var(--plane);color:var(--text-primary);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
.wrap{max-width:1000px;margin:0 auto;padding:32px 20px 80px}
h1{font-size:26px;margin:0 0 4px}h2{font-size:19px;margin:36px 0 6px;border-top:1px solid var(--border);padding-top:20px}
p.lead{color:var(--text-secondary);margin:4px 0 8px}.small{color:var(--muted);font-size:13px}
.fig{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:14px 12px 8px;margin:14px 0}
.fig h3{font-size:15px;margin:2px 6px 8px}.chart{width:100%;height:auto;display:block;overflow:visible}
.chart .grid{stroke:var(--grid);stroke-width:1}.chart .vmark{stroke:var(--axis);stroke-width:1;stroke-dasharray:3 3;opacity:.7}
.chart .tick{fill:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}.chart .vlab{fill:var(--muted);font-size:10px}
.chart .axlab{fill:var(--text-secondary);font-size:12px}.chart .slab{fill:var(--text-secondary);font-size:12px;font-weight:600}
.stats{display:flex;flex-wrap:wrap;gap:12px;margin:14px 0}
.stat{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:12px 16px;min-width:150px;flex:1}
.stat .sv{font-size:24px;font-weight:700;font-variant-numeric:tabular-nums}.stat .sl{color:var(--text-secondary);font-size:13px;margin-top:2px}.stat .sub{color:var(--muted);font-size:12px;margin-top:2px}
table.data{border-collapse:collapse;width:100%;font-size:12px;font-variant-numeric:tabular-nums;margin-top:8px}
table.data th,table.data td{border:1px solid var(--border);padding:3px 7px;text-align:right}table.data th{background:var(--surface-1);position:sticky;top:0}
details{margin-top:8px}summary{cursor:pointer;color:var(--text-secondary)}
code{background:var(--surface-1);border:1px solid var(--border);border-radius:5px;padding:1px 5px;font-size:12px}
.tblwrap{max-height:420px;overflow:auto;border:1px solid var(--border);border-radius:8px}
ul{margin:6px 0}li{margin:3px 0}
"""
    def fig(t, svg, note=""):
        return f'<div class="fig"><h3>{html.escape(t)}</h3><div style="overflow-x:auto">{svg}</div>{f"<p class=small>{note}</p>" if note else ""}</div>'

    body = f"""
<div class="wrap">
<h1>Online superblock learning curves — {html.escape(a.corpus)}</h1>
<p class="lead">Issue #16 line-dedup transport. A continuously-growing online predictor over stable marker-region IDs: each TU is encoded with the predictor state learned from <em>previously observed</em> TUs only (prequential), the full per-F wire is charged, then the TU is learned from. Immutable object IDs never rebind. Every point below is auditable in the committed TSVs.</p>
<p class="small">Generated {now} · {html.escape(gcc)} · {html.escape(host)} · commit <code>{html.escape(a.commit or 'see branch')}</code></p>
<div class="stats">{stats}</div>

<h2>1 · Cold → hot learning</h2>
<p class="lead">x-axis is <strong>TUs observed</strong> (prequential), continuing across repeated build loops. Dashed verticals mark phase boundaries (cold → warm loops → header-edit loops → revert). Primary metric is full charged wire (roots + newly-required Line/Block definitions + fills + framing, charged to the F that receives them). The warm loops replay the <em>identical</em> tree, so with a pure count-gate the learner keeps promoting deeper blocks each rebuild toward a degenerate ~1-token-per-TU floor; the informative signal is the <em>slope</em> (each identical rebuild roughly halves the root tokens) and the fact that it surpasses the single-build batch reference by loop ~2 (Q&gt;1) by accumulating cross-build structure a one-shot batch never sees.</p>
{fig("Effective compression: bits per input byte (full charged wire)", c1, "Cold loop pays first-time Line-text closure transfer; warm loops drop toward the steady floor. Per-TU MA128 shown faint.")}
{fig("Prequential predictor quality Q = (B−O)/(B−A)", c2, "B = marker-region baseline root code length (no blocks); O = online root code length using blocks published before the TU; A = retrospective batch region-BPE ceiling. 0 = no superblock benefit yet; 1 = batch reference. Unclamped: Q>1 means repeated online learning surpasses this batch reference.")}
{fig("Structure learning: root tokens per input region", c3, "How many top-level tokens the current predictor needs to cover one input region. Lower = more structure absorbed into blocks.")}

<h2>2 · Learner-state growth</h2>
{fig("Published immutable blocks vs TUs observed", c4, (f"Final: {life[0]} published, {life[1]} ever used, {life[3]} never used (max depth {life[4]}). Never-used blocks cost C-side predictor memory only — they are never sent to any F." if life else ""))}

<h2>3 · Per-F definition multiplication</h2>
<p class="lead">Definitions are charged once per F that actually receives them (closure-only, no global pre-send). Scattering one build across F caches (round-robin) means no single F is warm, so the cold Line-text closure is re-sent per F; sticky/affinity (return a build's TUs to the F holding its C_GUID) keeps it 1×.</p>
{fig("Cold-build definition transfer vs F count", c5, "round-robin multiplies ~×F; sticky stays at the single-F cost.")}

<h2>4 · Promotion-gate sweep</h2>
{fig("Warm covering vs promote threshold", c6, "Lower promote gate = closer to the batch covering but more blocks (and, on high-diversity corpora, more never-reused Zipfian-tail blocks). A full-cost gate replaces the raw count gate in the product design.")}

<h2>5 · Per-loop data (every plotted point)</h2>
<div class="tblwrap">{loop_table()}</div>
<p class="small">Raw machine-readable inputs on the branch: <code>{html.escape(a.prefix)}-pertu.tsv</code> (per-TU), <code>{html.escape(a.prefix)}-perloop.tsv</code> (per-loop). Regenerate: <code>python3 make_online_report.py --prefix {html.escape(a.prefix)} --log {html.escape(a.log)} --corpus {html.escape(a.corpus)} --out {html.escape(a.out)}</code></p>

<h2>Method &amp; provenance</h2>
<ul>
<li><strong>Observed vs modeled:</strong> root/def/missing byte counts and block/region/line structure are <em>observed</em> exactly (byte-exact: every TU's root tokens expand to its exact line-id stream, FNV-1a anchored to raw source). zstd sizes are observed per-TU (root, L3) and via a per-kind global ratio for definition closures (<em>modeled</em>). Framing is a fixed per-frame model ({int(12)} B root + {int(12)} B fill).</li>
<li><strong>Command:</strong> <code>{html.escape(a.cmd or 'superblock-online-bench --manifest … --promote 4 --sweep --curves '+a.prefix+' --max-loops 20')}</code></li>
<li><strong>Deferred panels</strong> (flagged, not yet plotted): LZ/phrase-trie alternative learner behind the same block store; full order-sensitivity matrix (reverse / scheduler-completion-order / fresh-shuffle-per-loop with seed bands); exact-DP covering control.</li>
</ul>
</div>
"""
    doc = f'<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Superblock Learning Curves</title><style>{css}</style></head><body>{body}</body></html>'
    with open(a.out, "w") as f:
        f.write(doc)
    print(f"wrote {a.out} ({len(doc)} bytes) from {len(perloop)} loops / {len(pertu)} per-TU rows")

if __name__ == "__main__":
    main()
