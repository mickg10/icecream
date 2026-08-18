#!/usr/bin/env python3
"""Four-line learning-curve join, one TU axis, per corpus.

Lines 1-4 share ONE basis: single-decoder cumulative C-side wire bytes.
  p29batch_*    codec50, the /goal batch configuration; endpoint == the /goal P29 wire
  grz2_*        grz2g-selector at the binding --gtu 112; endpoint == the /goal GRZ2 wire
  fastintern_*  production-fused (line dedup + z3), per TU
  pred_*        bakeoff empty-online-k2 -- COLD, no pretrained seed; only TUs 1..200 exist

Carried alongside on a DIFFERENT basis, never to be plotted against the four:
  p29socket_*   cap_m5 per-TU socket dialogue at one sticky consumer, + its Root/Need/Fill

Every axis claim is asserted, not assumed; a source that fails its check is dropped to NA
rather than silently misaligned.
"""
import os
import sys

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus6": "godot", "corpus9": "catch2",
      "corpus11": "range-v3", "corpus12": "eigen"}
W = os.path.expanduser("~/selbind/curves")
M5 = os.path.expanduser("~/selbind/m5split/runs")
BK = os.path.expanduser("~/bakeoff/curves_empty")


def rows(path, skip=1):
    return [l.split("\t") for l in open(path).read().splitlines()[skip:]]


for c in sys.argv[1:]:
    notes = []
    p29 = rows(W + "/%s.p29batch.tsv" % c)                 # tu raw wire cum_raw cum_wire ratio exact
    n = len(p29)
    raw = [int(r[1]) for r in p29]
    cumraw = [int(r[3]) for r in p29]

    # --- fast interner -------------------------------------------------------------
    fast = rows(W + "/%s.fusedcurve.tsv" % c)
    if len(fast) != n or any(int(fast[i][1]) != raw[i] for i in range(n)):
        sys.exit("%s: fast-interner axis disagrees with codec50" % c)

    # --- GRZ2, binding and fine ----------------------------------------------------
    def grz_steps(tag):
        p = W + "/%s.%s.tsv" % (c, tag)
        if not os.path.exists(p):
            return {}, 0, 0
        head = open(p).read().splitlines()[0].split("\t")
        ix = {k: i for i, k in enumerate(head)}
        run, closed = 0, {}
        for g in rows(p):
            run += int(g[ix["comp_bytes"]])
            closed[int(g[ix["tu_hi"]]) - 1] = run
            b = int(g[ix["tu_lo"]]) - 1
            if b >= 0 and int(g[ix["hist_base"]]) + int(g[ix["hist_extent"]]) != cumraw[b]:
                sys.exit("%s: GRZ2 %s axis disagrees at tu %d" % (c, tag, b))
        out = W + "/%s.%s.out" % (c, tag.replace("curve", "").replace("grz2fine", "grz2fine") or "grz2")
        return closed, run, 0

    gb, gb_run, _ = grz_steps("grz2curve")
    gf, gf_run, _ = grz_steps("grz2fine")
    gb_total = int(open(W + "/%s.grz2.out" % c).read().split("\t")[1])
    gf_total = int(open(W + "/%s.grz2fine.out" % c).read().split("\t")[1]) if gf else 0

    # --- online prediction (cold, no seed) -----------------------------------------
    pred, frozen = {}, {}
    bp = BK + "/%s.tsv" % c
    if os.path.exists(bp):
        for r in rows(bp):
            i = int(r[1]) - 1                              # bakeoff TUs are 1-based
            if i >= n:
                continue
            if int(r[8]) != cumraw[i]:
                notes.append("prediction axis mismatch at tu %d -> dropped" % i)
                pred, frozen = {}, {}
                break
            (pred if r[0] == "empty-online-k2" else frozen)[i] = int(r[9])
    if pred:
        notes.append("prediction covers TUs 0-%d of %d" % (max(pred), n))

    # --- cap_m5 socket dialogue (different basis) ----------------------------------
    sock = []
    sp = M5 + "/%s.w1.tsv" % c
    if os.path.exists(sp):
        s = rows(sp)
        if len(s) == n and all(int(s[i][3]) == raw[i] for i in range(n)):
            sock = s

    head = ("tu\traw\tcumulative_raw\t"
            "p29batch_wire\tp29batch_cum_wire\tp29batch_ratio\t"
            "grz2_cum_wire\tgrz2_group_closes\tgrz2_ratio\tgrz2_fine_cum_wire\t"
            "fastintern_wire\tfastintern_cum_wire\tfastintern_ratio\t"
            "pred_cum_wire\tpred_ratio\tpredfrozen_cum_wire\t"
            "p29socket_wire\tp29socket_cum_wire\tp29socket_ratio\t"
            "p29socket_b_root\tp29socket_b_need\tp29socket_b_fill")
    out, gcum, fcum = [head], 0, 0
    for i in range(n):
        if i in gb:
            gcum = gb[i]
        if i in gf:
            fcum = gf[i]
        if i == n - 1:                                     # stream framing owns no group
            gcum += gb_total - gb_run
            if gf_total:
                fcum += gf_total - gf_run
        cr, pw, pc = cumraw[i], int(p29[i][2]), int(p29[i][4])
        fw, fc = int(fast[i][2]), int(fast[i][4])
        pr = pred.get(i)
        row = ["%d" % i, "%d" % raw[i], "%d" % cr,
               "%d" % pw, "%d" % pc, "%.2f" % (cr / pc),
               "%d" % gcum, "%d" % (1 if i in gb else 0),
               "%.2f" % (cr / gcum) if gcum else "NA", "%d" % fcum,
               "%d" % fw, "%d" % fc, "%.2f" % (cr / fc),
               "%d" % pr if pr else "NA", "%.2f" % (cr / pr) if pr else "NA",
               "%d" % frozen[i] if i in frozen else "NA"]
        if sock:
            s = sock[i]
            row += [s[4], s[7], "%.2f" % (cr / int(s[7])), s[8], s[9], s[10]]
        else:
            row += ["NA"] * 6
        out.append("\t".join(row))
    open(W + "/%s.learning-curve.tsv" % c, "w").write("\n".join(out) + "\n")

    print("%-9s %-9s TUs=%4d  p29batch=%d (%.1fx)  grz2=%d (%.1fx)  fast=%d (%.1fx)  "
          "pred@200=%s  socket=%s"
          % (c, NM.get(c, c), n, gcum and int(p29[-1][4]), cumraw[-1] / int(p29[-1][4]),
             gcum, cumraw[-1] / gcum, int(fast[-1][4]), cumraw[-1] / int(fast[-1][4]),
             ("%d (%.1fx)" % (pred[199], cumraw[199] / pred[199])) if 199 in pred else "n/a",
             ("%d" % int(sock[-1][7])) if sock else "MISSING"))
    for x in notes:
        print("    note: %s" % x)
