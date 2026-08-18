#!/usr/bin/env python3
"""Bind the causal GRZ2 / P29+BSC selector over the ii-matrix docker cells.

Selection rule (causal, no hindsight): P29+BSC is the always-on baseline and always
ships; GRZ2 is kept only when its own C-encode met the raw/1e9 deadline; selected is
the smaller of the legal candidates.  Whether P29+BSC itself met the deadline is a
property of the whole scheme and is reported separately.

P29+BSC's printed C rate is a per-stream proxy that excludes its `loaded+interned`
phase (load_corpus + Interner::process).  That phase is encoder work, so the full C
rate -- process wall clock minus the decode share -- is reported alongside it.
"""
import collections
import glob
import os
import re
import statistics
import sys

DM = os.path.expanduser("~/selbind/dm")
PROFILES = ["debian-gcc", "conan-gcc", "linuxbrew", "fedora-clang-libcxx"]
C_FLOOR, F_FLOOR = 1.0, 0.5


def read(path):
    with open(path) as handle:
        return handle.read()


def grab(pattern, text, cast=float):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None


def series(pattern, paths, cast=float):
    out = []
    for p in sorted(paths):
        v = grab(pattern, read(p), cast)
        if v is not None:
            out.append(v)
    return out


def med(v):
    return statistics.median(v) if v else None


def fmt(v, d=3):
    return "NA" if v is None else f"{v:.{d}f}"


def cell(d):
    project, profile, tus, raw, z19 = read(f"{d}/cell.meta").strip().split("\t")
    tus, raw, z19 = int(tus), int(raw), int(z19)
    row = dict(project=project, profile=profile, tus=tus, raw=raw, wp_z19=z19)

    row["GRZ2_bytes"] = int(read(sorted(glob.glob(f"{d}/grz.j8.enc.*.out"))[0]).split("\t")[1])
    for tag, key in (("j8", "GRZ2_Cgbps_j8"), ("j1", "GRZ2_Cgbps_1T")):
        walls = [float(read(p)) for p in sorted(glob.glob(f"{d}/grz.{tag}.enc.*.wall"))]
        row[key] = med([raw / 1e9 / w for w in walls])
        row[key + "_codec"] = med([c / 1e9 for c in
                                   series(r"C=(\d+) B/s", glob.glob(f"{d}/grz.{tag}.enc.*.err"))])
    row["GRZ2_Fgbps_1T"] = med([f / 1e9 for f in
                                series(r"F=(\d+) B/s", glob.glob(f"{d}/grz.dec.*.err"))])
    row["GRZ2_exact"] = read(f"{d}/grz.exact").strip()
    shas = set(read(f"{d}/grz.wiresha").split())
    row["GRZ2_wire_stable"] = "YES" if len(shas) == 1 else "NO"
    row["GRZ2_wire_sha256"] = sorted(shas)[0]

    def p29(tag):
        outs = sorted(glob.glob(f"{d}/p29.{tag}.*.out"))
        errs = sorted(glob.glob(f"{d}/p29.{tag}.*.err"))
        walls = [float(read(p)) for p in sorted(glob.glob(f"{d}/p29.{tag}.*.wall"))]
        fs = series(r"F-decode ([0-9.]+) GB/s", errs)
        return dict(bytes=grab(r"TOTAL=(\d+)", read(outs[0]), int),
                    exact=all("byte-exact=OK" in read(p) for p in outs),
                    c=med(series(r"C-encode ([0-9.]+) GB/s", errs)), f=med(fs),
                    intern=med(series(r"loaded\+interned ([0-9.]+)s", errs)),
                    full=med([raw / 1e9 / (w - raw / 1e9 / x) for w, x in zip(walls, fs)]))

    nat, one = p29("native"), p29("1T")
    row.update(P29BSC_bytes=nat["bytes"], P29BSC_Cgbps_codec=nat["c"],
               P29BSC_Cgbps_full=nat["full"], P29BSC_Cgbps_full_1T=one["full"],
               P29BSC_Fgbps_1T=one["f"], P29BSC_intern_s=nat["intern"],
               P29BSC_intern_gbps=raw / 1e9 / nat["intern"] if nat["intern"] else None,
               P29BSC_exact="YES" if nat["exact"] and one["exact"] else "NO")
    return row


def main():
    rows = [cell(d) for d in sorted(glob.glob(f"{DM}/*")) if os.path.exists(f"{d}/cell.meta")]
    if not rows:
        sys.exit("no measured cells")

    cols = ["project", "profile", "tus", "raw", "wp_z19",
            "P29BSC_bytes", "P29BSC_Cgbps_full", "P29BSC_Cgbps_full_1T",
            "P29BSC_Cgbps_codec", "P29BSC_Fgbps_1T", "P29BSC_intern_gbps",
            "GRZ2_bytes", "GRZ2_Cgbps_j8", "GRZ2_Cgbps_1T", "GRZ2_Cgbps_j8_codec",
            "GRZ2_Fgbps_1T", "P29BSC_ratelegal", "GRZ2_ratelegal", "selected_codec",
            "selected_bytes", "selected_over_z19", "selected_Fgbps_1T", "selected_F_ok",
            "decode_exact", "GRZ2_wire_stable", "GRZ2_wire_sha256"]
    print("\t".join(cols))

    agg = collections.defaultdict(lambda: dict(raw=0, z19=0, sel=0, best=0, n=0,
                                               grz=0, p29=0, wins=collections.Counter(),
                                               floor=0, fmiss=0))
    for row in rows:
        gl = row["GRZ2_Cgbps_j8_codec"] >= C_FLOOR
        pl = row["P29BSC_Cgbps_full"] >= C_FLOOR
        if gl and row["GRZ2_bytes"] < row["P29BSC_bytes"]:
            codec, size = "GRZ2", row["GRZ2_bytes"]
        else:
            codec, size = "P29BSC", row["P29BSC_bytes"]
        sf = row[f"{codec}_Fgbps_1T"]
        row.update(P29BSC_ratelegal=pl, GRZ2_ratelegal=gl, selected_codec=codec,
                   selected_bytes=size, selected_over_z19=size / row["wp_z19"],
                   selected_Fgbps_1T=sf, selected_F_ok=sf >= F_FLOOR,
                   decode_exact=row[f"{codec}_exact"])
        print("\t".join(fmt(row[c]) if isinstance(row[c], float) else str(row[c])
                        for c in cols))
        for key in (row["profile"], "ALL"):
            a = agg[key]
            a["raw"] += row["raw"]; a["z19"] += row["wp_z19"]; a["sel"] += size
            a["grz"] += row["GRZ2_bytes"]; a["p29"] += row["P29BSC_bytes"]; a["n"] += 1
            a["best"] += min(row["GRZ2_bytes"], row["P29BSC_bytes"])
            a["wins"][codec] += 1
            a["floor"] += 0 if pl else 1
            a["fmiss"] += 0 if sf >= F_FLOOR else 1

    e = sys.stderr
    print(f"# cells={len(rows)}", file=e)
    for key in PROFILES + ["ALL"]:
        if key not in agg:
            continue
        a = agg[key]
        print(f"# {key:22s} n={a['n']:2d} raw={a['raw'] / 1e9:8.2f}GB "
              f"selected={a['sel']:>10d} /z19={a['sel'] / a['z19']:.4f} "
              f"x_raw={a['raw'] / a['sel']:7.2f}  hindsight_x={a['raw'] / a['best']:7.2f}  "
              f"GRZ2_wins={a['wins']['GRZ2']} P29BSC_wins={a['wins']['P29BSC']}  "
              f"P29BSC_floor_miss={a['floor']}/{a['n']}  selF_miss={a['fmiss']}/{a['n']}",
              file=e)
        print(f"#   {'':20s} GRZ2_only_x={a['raw'] / a['grz']:7.2f}  "
              f"P29BSC_only_x={a['raw'] / a['p29']:7.2f}  "
              f"z19_x={a['raw'] / a['z19']:7.2f}", file=e)


if __name__ == "__main__":
    main()
