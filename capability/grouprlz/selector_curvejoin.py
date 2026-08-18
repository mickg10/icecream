#!/usr/bin/env python3
"""Join the per-TU learning curves onto one TU axis, per corpus.

  tu, raw, cumulative_raw,
  fastintern_wire, fastintern_cum_wire,          (per TU, exact)
  grz2_cum_wire, grz2_group, grz2_group_closes,  (step: complete groups only)
  fastintern_ratio, grz2_ratio                   (cumulative_raw / cumulative_wire)

GRZ2 buffers whole groups, so its cumulative is only defined at group boundaries; the
column forward-fills the last CLOSED group, which is the honest "bytes actually
transmitted by the end of this TU". The stream framing that belongs to no group is added
to the final row so the curve terminates at the codec's true total.
"""
import os
import sys

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus6": "godot", "corpus9": "catch2",
      "corpus11": "range-v3", "corpus12": "eigen"}
W = os.path.expanduser("~/selbind/curves")

for c in sys.argv[1:]:
    fused = W + "/%s.fusedcurve.tsv" % c
    grz = W + "/%s.grz2curve.tsv" % c
    tot = W + "/%s.grz2.out" % c
    if not (os.path.exists(fused) and os.path.exists(grz)):
        print("skip %s (missing input)" % c, file=sys.stderr)
        continue
    frows = [l.split("\t") for l in open(fused).read().splitlines()[1:]]
    ghead = open(grz).read().splitlines()[0].split("\t")
    gi = {k: i for i, k in enumerate(ghead)}
    grows = [l.split("\t") for l in open(grz).read().splitlines()[1:]]
    grz_total = int(open(tot).read().split("\t")[1])

    # cumulative wire after each group closes, keyed by the TU index where it closes
    closed, run = {}, 0
    for g in grows:
        run += int(g[gi["comp_bytes"]])
        closed[int(g[gi["tu_hi"]]) - 1] = (run, int(g[gi["group"]]))
    group_sum = run

    # cross-check the two codecs really are on the same TU axis
    for g in grows[1:]:
        boundary = int(g[gi["tu_lo"]]) - 1
        want = int(g[gi["hist_base"]]) + int(g[gi["hist_extent"]])
        got = int(frows[boundary][3])
        if want != got:
            print("  AXIS MISMATCH %s at tu %d: grz2 %d vs fused %d"
                  % (c, boundary, want, got), file=sys.stderr)

    # optional fine-grained GRZ2 (--gtu 8): SHAPE ONLY, a different configuration
    fclosed, frun, fine_total = {}, 0, 0
    fp = W + "/%s.grz2fine.tsv" % c
    if os.path.exists(fp):
        fh = open(fp).read().splitlines()
        fidx = {k: i for i, k in enumerate(fh[0].split("\t"))}
        for l in fh[1:]:
            g = l.split("\t")
            frun += int(g[fidx["comp_bytes"]])
            fclosed[int(g[fidx["tu_hi"]]) - 1] = frun
        fine_total = int(open(W + "/%s.grz2fine.out" % c).read().split("\t")[1])

    # P29 line-interning per-TU wire (cap_m5, workers=1) with its Root/Need/Fill split
    M5 = os.path.expanduser("~/selbind/m5split/runs/%s.w1.tsv" % c)
    p29 = []
    if os.path.exists(M5):
        p29 = [l.split("\t") for l in open(M5).read().splitlines()[1:]]
        if len(p29) != len(frows):
            print("  P29 LENGTH MISMATCH %s: %d vs %d" % (c, len(p29), len(frows)),
                  file=sys.stderr)
            p29 = []
        else:
            for i, r in enumerate(p29):
                if int(r[3]) != int(frows[i][1]):
                    print("  P29 AXIS MISMATCH %s at tu %d" % (c, i), file=sys.stderr)
                    p29 = []
                    break

    out = [("tu\traw\tcumulative_raw\tfastintern_wire\tfastintern_cum_wire\t"
            "grz2_cum_wire\tgrz2_group\tgrz2_group_closes\tfastintern_ratio\tgrz2_ratio\t"
            "grz2_fine_cum_wire\tp29_wire\tp29_cum_wire\tp29_ratio\t"
            "p29_b_root\tp29_b_need\tp29_b_fill")]
    cum, grp, n, fcum = 0, -1, len(frows), 0
    for i, f in enumerate(frows):
        raw, cr, fw, fc = int(f[1]), int(f[3]), int(f[2]), int(f[4])
        hit = i in closed
        if hit:
            cum, grp = closed[i]
        if i in fclosed:
            fcum = fclosed[i]
        if i == n - 1:
            cum += grz_total - group_sum      # stream framing owned by no group
            if fine_total:
                fcum += fine_total - frun
        if p29:
            pw, pc = int(p29[i][4]), int(p29[i][7])
            ptail = "\t%d\t%d\t%.2f\t%s\t%s\t%s" % (
                pw, pc, cr / pc if pc else 0, p29[i][8], p29[i][9], p29[i][10])
        else:
            ptail = "\tNA\tNA\tNA\tNA\tNA\tNA"
        out.append("%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.2f\t%s\t%d%s"
                   % (i, raw, cr, fw, fc, cum, grp, 1 if hit else 0,
                      cr / fc if fc else 0, "%.2f" % (cr / cum) if cum else "NA", fcum, ptail))
    path = W + "/%s.learning-curve.tsv" % c
    open(path, "w").write("\n".join(out) + "\n")
    print("%-9s %-9s TUs=%d  fast_total=%d (%.1fx)  grz2_total=%d (%.1fx)  framing=%d"
          % (c, NM.get(c, c), n, int(frows[-1][4]), int(frows[-1][3]) / int(frows[-1][4]),
             cum, int(frows[-1][3]) / cum, grz_total - group_sum)
          + ("  fine(gtu8)=%d (+%.1f%%, %d pts)" % (fine_total, (fine_total / cum - 1) * 100, len(fclosed)) if fine_total else "")
          + ("  p29=%d (%.1fx)" % (int(p29[-1][7]), int(frows[-1][3]) / int(p29[-1][7])) if p29 else "  p29=MISSING"))
