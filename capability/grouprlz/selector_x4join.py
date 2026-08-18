#!/usr/bin/env python3
"""Join the 4-pass learning curves onto one TU axis, per project.

Columns: tu, pass, raw, cumulative_raw, then one cumulative-wire column per codec.
Every codec is asserted against codec50's per-TU raw sequence; a source that disagrees
is dropped to NA rather than silently misaligned.  GRZ2 is a step function (groups close
every 112 TUs) and is forward-filled from the last CLOSED group, as before.
"""
import os
import sys

X = os.path.expanduser("~/selbind/x4")
PRED = os.path.expanduser("~/selbind/pred4")


def rows(path, skip=1):
    return [l.split("\t") for l in open(path).read().splitlines()[skip:]]


for c in sys.argv[1:]:
    notes = []
    p29 = rows("%s/%s.p29.tsv" % (X, c))          # tu raw wire cum_raw cum_wire ratio exact
    n = len(p29)
    raw = [int(r[1]) for r in p29]
    cumraw = [int(r[3]) for r in p29]
    base_tus = n // 4
    if base_tus * 4 != n:
        notes.append("row count %d is not 4x a whole pass" % n)

    fast = rows("%s/%s.fast.tsv" % (X, c))         # tu raw wire cum_raw cum_wire ...
    if len(fast) != n or any(int(fast[i][1]) != raw[i] for i in range(n)):
        sys.exit("%s: fast-interner axis disagrees with codec50" % c)
    z3 = rows("%s/%s.zstd3.tsv" % (X, c))          # tu raw wire cum_raw cum_wire
    if len(z3) != n or any(int(z3[i][1]) != raw[i] for i in range(n)):
        sys.exit("%s: zstd3 axis disagrees with codec50" % c)

    # GRZ2 group steps, asserted at every boundary
    gb, run, gtot = {}, 0, 0
    gp = "%s/%s.grz2.tsv" % (X, c)
    if os.path.exists(gp):
        head = open(gp).read().splitlines()[0].split("\t")
        ix = {k: i for i, k in enumerate(head)}
        for g in rows(gp):
            run += int(g[ix["comp_bytes"]])
            gb[int(g[ix["tu_hi"]]) - 1] = run
            b = int(g[ix["tu_lo"]]) - 1
            if b >= 0 and int(g[ix["hist_base"]]) + int(g[ix["hist_extent"]]) != cumraw[b]:
                sys.exit("%s: GRZ2 axis disagrees at tu %d" % (c, b))
        gtot = int(open("%s/%s.grz2.out" % (X, c)).read().split("\t")[1])

    # prediction, if it exists for this project
    pred = {}
    pp = "%s/%s.pred.tsv" % (PRED, c)
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
        if pred:
            notes.append("prediction covers TUs 0-%d of %d" % (max(pred), n))
    else:
        notes.append("prediction MISSING")

    out = [("tu\tpass\traw\tcumulative_raw\tp29_cum_wire\tgrz2_cum_wire\t"
            "fastintern_cum_wire\tzstd3_cum_wire\tpred_cum_wire\tgrz2_group_closes")]
    gcum = 0
    for i in range(n):
        if i in gb:
            gcum = gb[i]
        if i == n - 1 and gtot:
            gcum += gtot - run                     # stream framing owns no group
        out.append("\t".join(str(v) for v in [
            i, i // base_tus + 1 if base_tus else 1, raw[i], cumraw[i],
            int(p29[i][4]), gcum if gcum else "NA", int(fast[i][4]), int(z3[i][4]),
            pred.get(i, "NA"), 1 if i in gb else 0]))
    path = "%s/%s.curve4.tsv" % (X, c)
    open(path, "w").write("\n".join(out) + "\n")

    print("%-9s TUs=%d (4x%d)  p29=%d  grz2=%s  fast=%d  zstd3=%d  pred=%s"
          % (c, n, base_tus, int(p29[-1][4]), gcum if gcum else "NA",
             int(fast[-1][4]), int(z3[-1][4]),
             pred.get(n - 1, "partial/NA")))
    for x in notes:
        print("    note: %s" % x)
