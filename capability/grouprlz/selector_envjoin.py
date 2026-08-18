#!/usr/bin/env python3
"""Join one (corpus x docker-env) cell's codec curves onto a single TU axis.

Panel layout is one panel per corpus, colour = codec, dash = docker env, so the unit of
data is (corpus, env) and each file carries the five codec columns over 4 passes.
"""
import os
import sys

W = os.path.expanduser("~/selbind/envx")
PRED = os.path.expanduser("~/selbind/predenv")


def rows(path, skip=1):
    return [l.split("\t") for l in open(path).read().splitlines()[skip:]]


for tag in sys.argv[1:]:
    notes = []
    p29 = rows("%s/%s.p29.tsv" % (W, tag))
    n = len(p29)
    raw = [int(r[1]) for r in p29]
    cumraw = [int(r[3]) for r in p29]
    base = n // 4
    if base * 4 != n:
        notes.append("row count %d is not 4x a whole pass" % n)

    fast = rows("%s/%s.fast.tsv" % (W, tag))
    if len(fast) != n or any(int(fast[i][1]) != raw[i] for i in range(n)):
        sys.exit("%s: fast-interner axis disagrees with codec50" % tag)
    z3 = rows("%s/%s.zstd3.tsv" % (W, tag))
    if len(z3) != n or any(int(z3[i][1]) != raw[i] for i in range(n)):
        sys.exit("%s: zstd3 axis disagrees with codec50" % tag)

    gb, run, gtot = {}, 0, 0
    gp = "%s/%s.grz2.tsv" % (W, tag)
    if os.path.exists(gp) and os.path.getsize(gp) > 0:
        head = open(gp).read().splitlines()[0].split("\t")
        ix = {k: i for i, k in enumerate(head)}
        for g in rows(gp):
            run += int(g[ix["comp_bytes"]])
            gb[int(g[ix["tu_hi"]]) - 1] = run
            b = int(g[ix["tu_lo"]]) - 1
            if b >= 0 and int(g[ix["hist_base"]]) + int(g[ix["hist_extent"]]) != cumraw[b]:
                sys.exit("%s: GRZ2 axis disagrees at tu %d" % (tag, b))
        try:
            gtot = int(open("%s/%s.grz2.out" % (W, tag)).read().split("\t")[1])
        except Exception:
            notes.append("GRZ2 total unreadable")
    else:
        notes.append("GRZ2 MISSING")

    pred = {}
    pp = "%s/%s.pred.tsv" % (PRED, tag)
    if os.path.exists(pp):
        ph = open(pp).read().splitlines()
        pix = {k: i for i, k in enumerate(ph[0].split("\t"))}
        for r in (l.split("\t") for l in ph[1:]):
            if r[0] != "empty-online-k2":
                continue
            i = int(r[pix["tu"]]) - 1
            if i < n and int(r[pix["cumulative_raw_bytes"]]) == cumraw[i]:
                pred[i] = int(r[pix["cumulative_charged_bytes"]])
            elif i < n:
                notes.append("prediction axis mismatch at tu %d -> dropped" % i)
                pred = {}
                break
    else:
        notes.append("prediction MISSING")

    proj, env = tag.split(".", 1)
    out = [("tu\tpass\tproject\tenv\traw\tcumulative_raw\tp29_cum_wire\tgrz2_cum_wire\t"
            "fastintern_cum_wire\tzstd3_cum_wire\tpred_cum_wire\tgrz2_group_closes")]
    gcum = 0
    for i in range(n):
        if i in gb:
            gcum = gb[i]
        if i == n - 1 and gtot:
            gcum += gtot - run
        out.append("\t".join(str(v) for v in [
            i, i // base + 1 if base else 1, proj, env, raw[i], cumraw[i],
            int(p29[i][4]), gcum if gcum else "NA", int(fast[i][4]), int(z3[i][4]),
            pred.get(i, "NA"), 1 if i in gb else 0]))
    open("%s/%s.curve4.tsv" % (W, tag), "w").write("\n".join(out) + "\n")
    print("%-28s TUs=%d (4x%d)  p29=%d  grz2=%s  fast=%d  zstd3=%d  pred=%s"
          % (tag, n, base, int(p29[-1][4]), gcum if gcum else "NA",
             int(fast[-1][4]), int(z3[-1][4]), pred.get(n - 1, "NA")))
    for x in notes:
        print("    note: %s" % x)
