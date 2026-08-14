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

def render_corpus(corpus, prefix, log):
    """Return (nav_label, section_html) for one corpus."""
    pertu = read_tsv(prefix + "-pertu.tsv")
    perloop = read_tsv(prefix + "-perloop.tsv")
    logtext = open(log).read() if log else ""
    L = parse_log(logtext)

    # loop boundaries (x at each loop end) + labels
    vlines = []
    for r in perloop:
        vlines.append((num(r["x_end"]), f'{r["phase"][0]}{r["loop"]}'))
    xmax = max(num(r["x_end"]) for r in perloop) if perloop else None
    # cold/warm/edit/revert phase boundaries only (fewer labels)
    phase_bounds = []; last = None
    for r in perloop:
        ph = "frozen" if r["phase"].startswith("frozen") else r["phase"]
        if ph != last:
            phase_bounds.append((num(r["x_end"]) - num(r["TUs"]), ph)); last = ph

    # per-loop series
    def loop_pts(col): return [(num(r["x_end"]), num(r[col])) for r in perloop]
    # per-TU rolling (bits/byte, tok/reg) for faint texture
    tu_bpb = [(num(r["x_obs"]), num(r["bits_per_byte"])) for r in pertu]
    tu_bpb_ma = downsample(moving_avg(tu_bpb, 128))
    tu_tr = [(num(r["x_obs"]), num(r["root_tok"]) / max(num(r["region_count"]), 1)) for r in pertu]
    tu_tr_ma = downsample(moving_avg(tu_tr, 128))

    # prequential Q_W(t) rolling windows: Q_W = sum_W(B_i-O_i) / sum_W(B_i-A_i), x = TUs observed.
    xs = [num(r["x_obs"]) for r in pertu]
    BmO = [num(r["B_root_z3"]) - num(r["O_root_z3"]) for r in pertu]
    BmA = [num(r["B_root_z3"]) - num(r["A_root_z3"]) for r in pertu]
    def rolling_Q(W):
        from collections import deque
        qn = deque(); qd = deque(); sn = 0.0; sd = 0.0; out = []
        for i in range(len(xs)):
            qn.append(BmO[i]); sn += BmO[i]; qd.append(BmA[i]); sd += BmA[i]
            if len(qn) > W: sn -= qn.popleft(); sd -= qd.popleft()
            out.append((xs[i], sn / sd if abs(sd) > 1e-9 else 0.0))
        return out
    qw128 = downsample(rolling_Q(128)); qw32 = downsample(rolling_Q(32)); qw8 = downsample(rolling_Q(8))

    # define-and-use root memoization: per-loop full wire vs memoized wire, as bits/input-byte.
    # per-loop raw bytes = sum_full_wire_z / bits_per_byte * 8 (invert the stored bits/byte).
    has_memo = bool(perloop) and "sum_full_wire_memo_z" in perloop[0]
    def loop_raw(r):
        bpb = num(r["bits_per_byte"]); fw = num(r["sum_full_wire_z"])
        return (8.0 * fw / bpb) if bpb > 1e-12 else 0.0

    # ---- charts ----
    # 1. effective compression (bits/input-byte), log y
    c1 = line_chart(
        [ {"name":"per-loop","color_idx":0,"pts":loop_pts("bits_per_byte"),"markers":True},
          {"name":"per-TU MA128","color_idx":2,"pts":tu_bpb_ma,"faint":True} ],
        "TUs observed (across loops)", "bits / input byte  (log, lower=better)",
        ylog=True, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: (f"{v:g}"),
        title="Effective end-to-end compression learning curve")
    # 2. prequential Q_W(t) rolling windows (128 = readable headline; 8/32 fainter). UNCLAMPED.
    c2 = line_chart(
        [ {"name":"W=128","color_idx":1,"pts":qw128,"markers":False},
          {"name":"W=32","color_idx":3,"pts":qw32,"faint":True},
          {"name":"W=8","color_idx":2,"pts":qw8,"faint":True} ],
        "TUs observed (prequential)", "Q_W = Σ(B−O)/Σ(B−A)  (1.0 = batch ref)",
        ylog=False, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: f"{v:.2f}",
        title="Prequential predictor-quality learning curve Q_W(t) — rolling windows")
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

    # 7. define-and-use root memoization: full wire vs memoized wire (bits/input-byte), log.
    memo_series = []
    if has_memo:
        pts_full = []; pts_memo = []
        for r in perloop:
            raw = loop_raw(r); x = num(r["x_end"])
            if raw > 0:
                pts_full.append((x, 8.0*num(r["sum_full_wire_z"])/raw))
                pts_memo.append((x, 8.0*num(r["sum_full_wire_memo_z"])/raw))
        memo_series = [ {"name":"stack-greedy","color_idx":0,"pts":pts_full,"markers":True},
                        {"name":"+root memo","color_idx":1,"pts":pts_memo,"markers":True} ]
    c7 = line_chart(memo_series, "TUs observed (across loops)", "bits / input byte (log)",
                    ylog=True, xmax=xmax, vlines=phase_bounds, yfmt=lambda v:f"{v:g}",
                    title="Define-and-use whole-TU root memoization") if memo_series else "<p>(no memo data)</p>"

    # 8. promotion-gate comparison — count sweep vs full-cost points, BOTH from the same curve-driver
    # warm-last metric (gate-summary.tsv). Plotted as (blocks, tok/region): down-left is better.
    c8 = ""
    try:
        gs = read_tsv("gate-summary.tsv")
        cn = sorted([(int(r["blocks"]), float(r["tok_per_reg"])) for r in gs if r["corpus"]==corpus.lower() and r["gate"]=="count" and r["blocks"]])
        fc = sorted([(int(r["blocks"]), float(r["tok_per_reg"])) for r in gs if r["corpus"]==corpus.lower() and r["gate"]=="fullcost" and r["blocks"]])
        ser = []
        if len(cn)>=2: ser.append({"name":"count gate (promote 2..32)","color_idx":0,"pts":cn,"markers":True})
        elif cn: ser.append({"name":"count gate","color_idx":0,"pts":cn,"markers":True})
        if fc: ser.append({"name":"full-cost (fanout 1..8)","color_idx":1,"pts":fc,"markers":True})
        if ser:
            c8 = line_chart(ser, "published blocks", "warm tok/region",
                            ylog=False, xmax=max(b for b,_ in (cn+fc)), yfmt=lambda v:f"{v:.3f}",
                            title="Promotion gate: blocks vs covering (count vs full-cost, same metric)")
    except Exception:
        c8 = ""

    # 9. generation pruning: published (cumulative) vs resident (used within one build window) blocks.
    has_res = bool(perloop) and "resident_blk" in perloop[0]
    c9 = ""
    if has_res:
        c9 = line_chart(
            [ {"name":"published (cumulative)","color_idx":3,"pts":loop_pts("blocks_pub"),"markers":True},
              {"name":"resident after pruning","color_idx":0,"pts":loop_pts("resident_blk"),"markers":True},
              {"name":"never-reused","color_idx":1,"pts":loop_pts("never_blk"),"faint":True} ],
            "TUs observed (across loops)", "immutable blocks",
            ylog=False, xmax=xmax, vlines=phase_bounds, yfmt=lambda v: f"{int(v)}",
            title="Generation pruning: published vs resident block set")

    # ---- metadata ----
    hdr = L.get("hdr"); ceil = L.get("ceiling"); life = L.get("life")

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
            stat("corpus", corpus, f'{hdr[0]} TUs · {hdr[1]} MiB source'),
            stat("cold → 1st rebuild tok/region", f'{num(cold["tok_per_reg"]):.3f} → {num(warm1["tok_per_reg"]):.3f}', 'keeps halving per identical rebuild'),
            stat("1st-rebuild bits/input-byte", f'{bpb1:.5f}', f'≈ {1/max(bpb1/8,1e-12):.0f}× effective compression'),
            stat("wire vs cold", f'{num(cold["bits_per_byte"])/max(bpb1,1e-12):.0f}×', 'smaller after one warm rebuild'),
            stat("published blocks (final)", str(int(num(perloop[-1]["blocks_pub"]))) if perloop else "?", 'count-gate; Zipfian tail mostly unused — see §4'),
        ])

    def fig(t, svg, note=""):
        return f'<div class="fig"><h4>{html.escape(t)}</h4><div style="overflow-x:auto">{svg}</div>{f"<p class=small>{note}</p>" if note else ""}</div>'

    cid = re.sub(r"[^A-Za-z0-9]", "", corpus)
    # frozen-phase define-and-use win: unchanged rebuild collapses to ~1 ROOT_REF/TU
    memo_stat = ""
    fz = next((r for r in perloop if r["phase"]=="frozen_warm" and r["loop"]=="2"), None)
    fzr = next((r for r in perloop if r["phase"]=="frozen_revert"), None)
    if fz and has_memo:
        full = num(fz["sum_full_wire_z"]); memo = num(fz["sum_full_wire_memo_z"]); tus = num(fz["TUs"])
        rtok = num(fz["sum_root_tok"]); mtok = num(fz["sum_root_tok_memo"])
        rev = (f" On post-edit REVERT the old roots are instantly reusable ({num(fzr['sum_full_wire_z'])/max(num(fzr['sum_full_wire_memo_z']),1e-9):.1f}× smaller with memo)." if fzr else "")
        memo_stat = (f"<strong>Frozen deployed-dictionary result:</strong> an unchanged rebuild goes from {rtok/max(tus,1):.0f} to {mtok/max(tus,1):.1f} root tokens/TU (≈1 ROOT_REF), cutting full wire {full/max(memo,1e-9):.1f}× ({memo/max(tus,1):.0f} B/TU).{rev}")
    # generation-pruning memory stat: at the last WARM loop, resident vs published + reclaimable memory.
    prune_stat = ""
    if has_res:
        wl = next((r for r in reversed(perloop) if r["phase"]=="warm"), None)
        if wl:
            pub = num(wl["blocks_pub"]); res = num(wl["resident_blk"]); nev = num(wl["never_blk"]); rec = num(wl["reclaim_def_bytes"])
            # reclaimable predictor memory ≈ per-block slot (~40 B for {L,R,leaf,depth,created,uses,first,last}) × never-reused
            slot = 40.0
            prune_stat = (f"<strong>Generation pruning:</strong> at the warm plateau {int(pub)} blocks are published but only {int(res)} are resident (referenced within the last build); {int(nev)} ({100*nev/max(pub,1):.0f}%) were never reused — dropping those reclaims ~{nev*slot/1048576:.1f} MiB of C-side predictor memory (def bytes {rec/1024:.0f} KiB) at zero wire cost. Generation rotation bounds the resident set to the actively-used blocks instead of the unbounded published total.")
    final_blocks = int(num(perloop[-1]["blocks_pub"])) if perloop else 0
    life_note = (f"Published blocks grow to {final_blocks} across all loops. Under the raw count-gate most late promotions are never reused (C-side predictor memory only — never sent to any F); a full-cost gate bounds this. In a separate 4-pass run the used set held ~{life[1]} of {life[0]} — see §4." if life else f"Grows to {final_blocks} published blocks; count-gate over-promotes the Zipfian tail (§4).")
    section = f"""
<section id="c-{cid}">
<h2>{html.escape(corpus)}</h2>
<div class="stats">{stats}</div>
<h3>1 · Cold → hot learning</h3>
<p class="lead">x-axis = <strong>TUs observed</strong> (prequential), continuing across repeated build loops; dashed verticals mark phase boundaries (cold → warm → header-edit → revert → frozen). Warm loops replay the <em>identical</em> tree, so a pure count-gate keeps promoting deeper blocks toward a degenerate ~1-token/TU floor — the informative signal is the <em>slope</em> and the crossing of the single-build batch reference (Q&gt;1) as cross-build structure accumulates.</p>
{fig("Effective compression: bits per input byte (full charged wire)", c1, "Cold loop pays first-time Line-text closure transfer; warm loops drop toward the steady floor. Per-TU MA128 faint.")}
{fig("Prequential predictor quality Q_W(t) = Σ(B−O)/Σ(B−A), rolling windows", c2, "B = marker-region baseline (no blocks); O = online root code length using blocks published before the TU; A = single-build batch region-BPE reference. W=128 is the readable headline (8/32 faint). Unclamped: Q>1 = online surpasses that single-build batch reference by accumulating cross-build structure.")}
{fig("Structure learning: root tokens per input region", c3, "Top-level tokens the current predictor needs to cover one input region. Lower = more structure absorbed into blocks.")}
<p class="small"><strong>Read the cold-build numbers carefully:</strong> cold cumulative <em>wire</em> regret vs the batch ceiling is tiny (≈+0.4% on LLVM) — but only because cold wire is dominated by one-time Line-text definitions that BOTH the online learner and the ceiling must pay. The real structure-learning gap lives in ROOT TOKENS (roughly +75% LLVM / +17% RocksDB / +42% DuckDB early), and the blocks' payoff is the WARM steady state, not the cold build. The bits/byte panel above is full charged wire; the tok/region panel is the structure signal.</p>
<h3>2 · Learner-state growth &amp; generation pruning</h3>
{fig("Published immutable blocks vs TUs observed", c4, life_note)}
<p class="lead">{prune_stat}</p>
{fig("Generation pruning: published vs resident block set", c9, "Published grows unbounded on identical rebuilds; the resident set (blocks referenced within the last build window) and the never-reused count show what generation rotation reclaims. This — with the root-memo in §4 — is one of the two real memory bounds (the per-pair gate in §5 is not).") if c9 else ""}
<h3>3 · Per-F definition multiplication</h3>
{fig("Cold-build definition transfer vs F count", c5, "Charged once per F that receives it (closure-only). Round-robin multiplies ~×F (no single F is warm); sticky/affinity stays 1×.")}
<h3>4 · Define-and-use whole-TU root memoization</h3>
<p class="lead">On first sight of a TU, its whole root-token vector is labelled a root object (the child vector already crosses the wire, so the definition costs only a header); an identical later TU collapses to ONE <code>ROOT_REF</code>. It authorizes reuse only on exact object-sequence equality. {memo_stat}</p>
{fig("Full charged wire: stack-greedy vs +root-memo", c7, "During active learning the predictor churns the tokenization, so identical TUs rarely repeat their exact root vector and memo barely helps. In the FROZEN phase (deployed steady dictionary, learning off) an unchanged rebuild collapses to ~1 ROOT_REF/TU, and the post-edit REVERT is instantly reusable — the big memo/​no-memo gaps at the right.")}
<h3>5 · Promotion gate: count vs full-cost</h3>
{fig("Warm covering vs promote threshold (count gate)", c6, "Count-gate sweep: lower gate = closer covering but more blocks, and on high-diversity corpora a large never-reused Zipfian tail.")}
{fig("Blocks vs covering: count sweep vs full-cost rent-or-buy", c8, "Full-cost gate (publish a pair only when accrued foregone root-token savings ≥ def-bytes × per-F fan-out) BOUNDS the block count sharply, but on this data it does NOT dominate the count-gate frontier — a tuned count threshold reaches similar covering at similar block counts. Honest read: the per-pair gate shifts the operating point; the decisive memory bounds are generation pruning + the root-memo above, not a cleverer per-pair rule.") if c8 else ""}
<h3>6 · Per-loop data (every plotted point)</h3>
<div class="tblwrap">{loop_table()}</div>
<p class="small">Machine-readable inputs on the branch: <code>{html.escape(prefix)}-pertu.tsv</code>, <code>{html.escape(prefix)}-perloop.tsv</code>.</p>
</section>
"""
    return corpus, section

CSS = """
:root{--surface-1:#fcfcfb;--plane:#f9f9f7;--text-primary:#0b0b0b;--text-secondary:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;--border:rgba(11,11,11,.10);--s0:#2a78d6;--s1:#eb6834;--s2:#1baf7a;--s3:#eda100;}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--surface-1:#1a1a19;--plane:#0d0d0d;--text-primary:#fff;--text-secondary:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--border:rgba(255,255,255,.10);--s0:#3987e5;--s1:#d95926;--s2:#199e70;--s3:#c98500;}}
:root[data-theme=dark]{--surface-1:#1a1a19;--plane:#0d0d0d;--text-primary:#fff;--text-secondary:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--border:rgba(255,255,255,.10);--s0:#3987e5;--s1:#d95926;--s2:#199e70;--s3:#c98500;}
*{box-sizing:border-box}body{margin:0;background:var(--plane);color:var(--text-primary);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
.wrap{max-width:1000px;margin:0 auto;padding:28px 20px 80px}
h1{font-size:27px;margin:0 0 4px;text-wrap:balance}
h2{font-size:22px;margin:14px 0 8px;padding:16px 0 6px;border-top:2px solid var(--border)}
h3{font-size:17px;margin:26px 0 4px}h4{font-size:14px;margin:2px 6px 8px}
p.lead{color:var(--text-secondary);margin:4px 0 8px}.small{color:var(--muted);font-size:13px}
nav.corpusnav{position:sticky;top:0;background:var(--plane);z-index:5;display:flex;gap:8px;flex-wrap:wrap;padding:10px 0;margin:6px 0 4px;border-bottom:1px solid var(--border)}
nav.corpusnav a{text-decoration:none;color:var(--text-secondary);border:1px solid var(--border);border-radius:999px;padding:3px 12px;font-size:13px}
nav.corpusnav a:hover{color:var(--text-primary);border-color:var(--axis)}
.fig{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:14px 12px 8px;margin:14px 0}
.chart{width:100%;height:auto;display:block;overflow:visible}
.chart .grid{stroke:var(--grid);stroke-width:1}.chart .vmark{stroke:var(--axis);stroke-width:1;stroke-dasharray:3 3;opacity:.7}
.chart .tick{fill:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}.chart .vlab{fill:var(--muted);font-size:10px}
.chart .axlab{fill:var(--text-secondary);font-size:12px}.chart .slab{fill:var(--text-secondary);font-size:12px;font-weight:600}
.stats{display:flex;flex-wrap:wrap;gap:12px;margin:14px 0}
.stat{background:var(--surface-1);border:1px solid var(--border);border-radius:12px;padding:12px 16px;min-width:150px;flex:1}
.stat .sv{font-size:23px;font-weight:700;font-variant-numeric:tabular-nums}.stat .sl{color:var(--text-secondary);font-size:13px;margin-top:2px}.stat .sub{color:var(--muted);font-size:12px;margin-top:2px}
table.data{border-collapse:collapse;width:100%;font-size:12px;font-variant-numeric:tabular-nums;margin-top:8px}
table.data th,table.data td{border:1px solid var(--border);padding:3px 7px;text-align:right}table.data th{background:var(--surface-1);position:sticky;top:0}
code{background:var(--surface-1);border:1px solid var(--border);border-radius:5px;padding:1px 5px;font-size:12px}
.tblwrap{max-height:420px;overflow:auto;border:1px solid var(--border);border-radius:8px}
ul{margin:6px 0}li{margin:4px 0}
"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--add", action="append", default=[], help="Name:prefix:log (repeatable)")
    # single-corpus back-compat
    ap.add_argument("--prefix"); ap.add_argument("--log"); ap.add_argument("--corpus")
    ap.add_argument("--commit", default=""); ap.add_argument("--cmd", default="")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    triples = []
    for s in a.add:
        name, prefix, log = s.split(":", 2)
        triples.append((name, prefix, log))
    if a.prefix and a.log and a.corpus:
        triples.append((a.corpus, a.prefix, a.log))
    if not triples:
        sys.exit("need --add Name:prefix:log (repeatable) or --corpus/--prefix/--log")

    try: gcc = subprocess.check_output(["g++","--version"]).decode().splitlines()[0]
    except Exception: gcc = "g++ (unknown)"
    try: host = subprocess.check_output(["uname","-srm"]).decode().strip()
    except Exception: host = "linux"
    now = datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%MZ")

    sections = []; navs = []
    for (name, prefix, log) in triples:
        label, sec = render_corpus(name, prefix, log)
        cid = re.sub(r"[^A-Za-z0-9]", "", name)
        navs.append(f'<a href="#c-{cid}">{html.escape(label)}</a>')
        sections.append(sec)

    intro = f"""
<div class="wrap">
<h1>Online superblock learning curves</h1>
<p class="lead">Issue #16 line-dedup transport. The superblock layer is a <strong>continuously-growing online predictor</strong> over stable marker-region IDs: each TU is encoded with the predictor state learned from <em>previously observed</em> TUs only (prequential), the full per-F wire is charged, then the TU is learned from and newly-worthwhile region/block pairs are promoted into <strong>new immutable Block IDs</strong> (existing IDs never rebind) available to later TUs. Passes: COLD → WARM loops (identical rebuilds) → one high-fanout header-edit → REVERT. Every plotted point is auditable in the committed per-TU/per-loop TSVs; wire is byte-exact (root tokens expand to the exact line-id stream).</p>
<p class="small">Generated {now} · {html.escape(gcc)} · {html.escape(host)} · commit <code>{html.escape(a.commit or 'see branch')}</code> · primary metric = full charged wire (defs charged once per F that receives them)</p>
<nav class="corpusnav">{''.join(navs)}</nav>
{''.join(sections)}
<h2>Method &amp; provenance</h2>
<ul>
<li><strong>Observed vs modeled:</strong> root/def/missing byte counts and block/region/line structure are <em>observed</em> exactly; zstd sizes are observed per-TU (root, L3) and modeled via a per-kind global ratio for definition closures; framing is a fixed per-frame model (12 B root + 12 B fill).</li>
<li><strong>Prequential rule:</strong> a TU is scored with blocks published <em>before</em> it; only after scoring is it learned from — so new knowledge from a TU never improves its own point. x continues across all loops.</li>
<li><strong>Regenerate:</strong> <code>{html.escape(a.cmd or 'superblock-online-bench --manifest <corpus>/manifest.txt --promote 4 --sweep --curves <prefix> --max-loops 20')}</code> then <code>python3 make_online_report.py --add Name:prefix:log ... --out superblock-online-report.html</code></li>
<li><strong>Covering control (min-wire):</strong> for each sample TU we produce candidate parses — stack-greedy LEFT-to-right (the deployed encoder) vs RIGHT-to-left — actually zstd each root stream and keep the smaller (bigoracle's k-best-with-real-zstd). Both parses are byte-exact. Result: RL is far worse (LLVM 270,442 vs 2,462 tokens) because the grammar is LR-learned, so best-of-candidates = LR within ~0.03%. The greedy LR encoder is self-consistent with its co-learned grammar; there is no free wire for a smarter covering to recover. An exact-DP min-token optimum over the full recursive block grammar is intractable at scale (deep whole-TU blocks blow up the CYK/trie), which is why k-best-with-real-zstd is the practical control.</li>
<li><strong>Independently cross-validated</strong> (separate agent, committed f6c18c9): order-sensitivity (≤0.46% wire spread across original/reverse/shuffled → order-invariant with a frozen store) and the pair-promotion-vs-LZ/phrase-trie comparison (pair-promotion uses 13–20× fewer blocks at equal-or-better wire). Not duplicated here.</li>
</ul>
</div>
"""
    doc = f'<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Superblock Learning Curves</title><style>{CSS}</style></head><body>{intro}</body></html>'
    with open(a.out, "w") as f:
        f.write(doc)
    print(f"wrote {a.out} ({len(doc)} bytes) · {len(triples)} corpora: {', '.join(t[0] for t in triples)}")

if __name__ == "__main__":
    main()
