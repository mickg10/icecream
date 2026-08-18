#!/usr/bin/env python3
"""Add an in-stream build-boundary flush to grz2g.

The prefix method was rejected, correctly: encoding each prefix independently changes the
group EXTENT (1x caps group 0 at the corpus end, 2x at --gtu), so earlier bytes are
rewritten and total(k)-total(k-1) is not the physical wire after a closed build.

This adds `--build-tus N`: the group window is additionally clamped to the next multiple of
N, so a group ALWAYS closes exactly on a build boundary inside ONE continuing stream.  The
matcher and dictionary state are untouched -- only the group closes -- and the absolute byte
offset at each close is recorded, so per-build bytes come from offsets in one stream.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
applied = []


def sub(tag, old, new, count=1):
    global t
    if t.count(old) != count:
        sys.exit("anchor %s matched %d times, expected %d" % (tag, t.count(old), count))
    t = t.replace(old, new, count)
    applied.append(tag)


sub("cfg", """    int select = 0;                      // 1 = per-group actual-byte backend selection
};""", """    int select = 0;                      // 1 = per-group actual-byte backend selection
    u64 build_tus = 0;                   // force a group close at every multiple of this
};""")

sub("arg", """        else if (!strcmp(o, "--curve")) curve = v;""",
    """        else if (!strcmp(o, "--curve")) curve = v;
        else if (!strcmp(o, "--build-tus")) c.build_tus = atoll(v);""")

sub("usage", """                 [--anchor-budget MB] [--select 1] [--retry-test n] [--curve f.tsv]\\n\"""",
    """                 [--anchor-budget MB] [--select 1] [--retry-test n] [--curve f.tsv]\\n"
            "                 [--build-tus n]   (close a group exactly at every n TUs)\\n\"""")

# the clamp itself
sub("clamp", """            u64 lim = std::min(tu_lo + cfg.gtu, ntu);
            tu_hi = tu_lo + 1;
            while (tu_hi < lim && tu[tu_hi + 1] - gpos <= cfg.graw) tu_hi++;
            closed = (tu_hi == lim) ? "tu" : "raw";""",
    """            u64 lim = std::min(tu_lo + cfg.gtu, ntu);
            // A build boundary is a hard close: the group must end there so the build is
            // complete and independently decodable at its own last TU.  Only the GROUP
            // closes; matcher and dictionary state carry forward untouched.
            bool at_build = false;
            if (cfg.build_tus) {
                u64 nb = (tu_lo / cfg.build_tus + 1) * cfg.build_tus;
                if (nb < lim) { lim = nb; at_build = true; }
                else if (nb == lim) at_build = true;
            }
            tu_hi = tu_lo + 1;
            while (tu_hi < lim && tu[tu_hi + 1] - gpos <= cfg.graw) tu_hi++;
            closed = (tu_hi == lim) ? (at_build ? "build" : "tu") : "raw";""")

sub("stat", """struct GroupStat {
    u64 idx, tu_lo, tu_hi, out_bytes, add_bytes, comp_bytes, hist_base, hist_extent;
    const char* closed_by;
};""", """struct GroupStat {
    u64 idx, tu_lo, tu_hi, out_bytes, add_bytes, comp_bytes, hist_base, hist_extent;
    const char* closed_by;
    u64 end_offset;      // absolute byte offset in THIS stream after the group is written
};""")

sub("push", """        curves.push_back({gidx, tu_lo, tu_hi, gout, (u64)lits.size(), w.n - before,
                          hist_base, gpos - hist_base, closed});""",
    """        curves.push_back({gidx, tu_lo, tu_hi, gout, (u64)lits.size(), w.n - before,
                          hist_base, gpos - hist_base, closed, w.n});""")

sub("header", """            fprintf(cf, "group\\ttu_lo\\ttu_hi\\tout_bytes\\tadd_bytes\\tcomp_bytes\\tratio\\thist_base\\thist_extent\\tclosed_by\\n");""",
    """            fprintf(cf, "group\\ttu_lo\\ttu_hi\\tout_bytes\\tadd_bytes\\tcomp_bytes\\tratio\\thist_base\\thist_extent\\tclosed_by\\tend_offset\\n");""")

sub("row", """                        (unsigned long long)g.hist_base, (unsigned long long)g.hist_extent, g.closed_by);""",
    """                        (unsigned long long)g.hist_base, (unsigned long long)g.hist_extent, g.closed_by,
                        (unsigned long long)g.end_offset);""")

sub("fmt", """                fprintf(cf, "%llu\\t%llu\\t%llu\\t%llu\\t%llu\\t%llu\\t%.2f\\t%llu\\t%llu\\t%s\\n",""",
    """                fprintf(cf, "%llu\\t%llu\\t%llu\\t%llu\\t%llu\\t%llu\\t%.2f\\t%llu\\t%llu\\t%s\\t%llu\\n",""")

open(dst, "w").write(t)
print("applied:", " ".join(applied))
