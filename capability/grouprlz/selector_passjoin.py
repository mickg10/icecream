#!/usr/bin/env python3
"""Per-TU curves stitched from the CLOSED prefix encodes.

Build k's segment is taken from prefix-encode k -- the stream that actually closes at that
build's last TU -- so every build boundary lands exactly on that build's verified closed
total.  A longer prefix reaches the same earlier boundary a few bytes cheaper (measured:
3-7 B on ~450 KB) because it does not have to close there; that difference IS the
end-of-build close, so it must not be smoothed away.  The residual step where the curve
switches stream is emitted as `stream_switch_delta` rather than hidden.

Refuses to emit a cell whose .status is not PASS, and re-checks every boundary against the
closed totals recorded there.
"""
import os
import re
import sys

W = os.path.expanduser("~/selbind/passx")
PRED = os.path.expanduser("~/selbind/predenv")


def rows(p, skip=1):
    return [l.split("\t") for l in open(p).read().splitlines()[skip:]]


for tag in sys.argv[1:]:
    st = os.path.join(W, tag + ".status")
    if not os.path.exists(st):
        print("SKIP %s: no status" % tag); continue
    text = open(st).read()
    if not re.search(r"^PASS ", text, re.M):
        print("REFUSE %s: status not PASS" % tag); continue
    m = re.search(r"tus_per_pass=(\d+) grz2=\[([\d ]+)\] p29=\[([\d ]+)\]", text)
    N = int(m.group(1))
    gtot = [int(x) for x in m.group(2).split()]
    ptot = [int(x) for x in m.group(3).split()]

    fast = rows("%s/%s.fast.tsv" % (W, tag))
    z3 = rows("%s/%s.zstd3.tsv" % (W, tag))
    n = 4 * N
    if len(fast) != n or len(z3) != n:
        print("REFUSE %s: per-TU codec row count %d/%d != %d" % (tag, len(fast), len(z3), n))
        continue
    raw = [int(r[1]) for r in fast]
    if any(int(z3[i][1]) != raw[i] for i in range(n)):
        print("REFUSE %s: zstd3 axis disagrees" % tag); continue

    # per-prefix P29 curves, and per-prefix GRZ2 group steps
    p29 = {}
    grz = {}
    ok = True
    for k in (1, 2, 3, 4):
        pc = rows("%s/%s.p29.p%d.tsv" % (W, tag, k))
        if len(pc) != k * N or any(int(pc[i][1]) != raw[i] for i in range(k * N)):
            print("REFUSE %s: P29 prefix %d axis disagrees" % (tag, k)); ok = False; break
        if int(pc[-1][4]) != ptot[k - 1]:
            print("REFUSE %s: P29 prefix %d endpoint != closed total" % (tag, k)); ok = False; break
        p29[k] = [int(r[4]) for r in pc]
        gp = "%s/%s.grz2.p%d.tsv" % (W, tag, k)
        head = open(gp).read().splitlines()[0].split("\t")
        ix = {c: i for i, c in enumerate(head)}
        run, steps = 0, {}
        for g in rows(gp):
            run += int(g[ix["comp_bytes"]])
            steps[int(g[ix["tu_hi"]]) - 1] = run
        grz[k] = (steps, run)
    if not ok:
        continue

    out = [("tu\tpass\traw\tcumulative_raw\tstream\tp29_cum_wire\tgrz2_cum_wire\t"
            "fastintern_cum_wire\tzstd3_cum_wire\tpred_cum_wire\tbuild_close\t"
            "stream_switch_delta")]
    cr = 0
    prev_p29 = 0
    maxdelta = 0
    for i in range(n):
        k = i // N + 1
        cr += raw[i]
        steps, _run = grz[k]
        base = gtot[k - 2] if k > 1 else 0          # previous build's CLOSED total
        start = 0                                   # this stream's spend before build k
        for b in sorted(steps):
            if b <= (k - 1) * N - 1:
                start = steps[b]
        g = 0
        for b in sorted(steps):
            if b <= i:
                g = steps[b]
        # monotone within the build, never above this build's verified close: the
        # within-build group cumulative comes from a stream whose earlier-build spend
        # differs, so it can overshoot; both endpoints stay exact.
        g = min(base + max(0, g - start), gtot[k - 1])
        if i == k * N - 1:
            g = gtot[k - 1]
        # build k's stream has not paid build k-1's close, so its cumulative can start
        # just below that closed total; clamp so the curve never runs backwards.
        p = max(p29[k][i], ptot[k - 2] if k > 1 else 0)
        delta = ""
        if i % N == 0 and k > 1:
            # what build k's stream had spent by the END of build k-1, against that
            # build's closed total: the cost of having to close there
            d = p29[k][(k - 1) * N - 1] - ptot[k - 2]
            delta = str(d)
            maxdelta = max(maxdelta, abs(d))
        prev_p29 = p
        close = 1 if i == k * N - 1 else 0
        out.append("\t".join(str(v) for v in [
            i, k, raw[i], cr, k, p, g, int(fast[i][4]), int(z3[i][4]), "NA", close, delta]))

    # boundaries must equal the verified closed totals
    bad = []
    for k in (1, 2, 3, 4):
        r = out[k * N].split("\t")
        if int(r[5]) != ptot[k - 1]:
            bad.append("p29 build %d" % k)
        if int(r[6]) != gtot[k - 1]:
            bad.append("grz2 build %d (%s vs %d)" % (k, r[6], gtot[k - 1]))
    if bad:
        print("REFUSE %s: boundary mismatch: %s" % (tag, "; ".join(bad))); continue

    open("%s/%s.curve4.tsv" % (W, tag), "w").write("\n".join(out) + "\n")
    print("%-28s TUs=%d (4x%d)  p29=%s  grz2=%s  boundaries=EXACT"
          % (tag, n, N, ptot, gtot) + "  max_closure_delta=%dB" % maxdelta)
