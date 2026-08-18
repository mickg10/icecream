#!/usr/bin/env python3
"""Bind the causal GRZ2-vs-P29+BSC selector on the fixed-16 corpora.

Reads the per-corpus evidence written by cell.sh and emits one TSV row per corpus.

Rate accounting
---------------
Every number here is measured on a warm page cache, so neither codec pays for the
NVMe read of its own input.

GRZ2 encodes in one process: its wall clock is the whole encoder.
P29+BSC's codec prints a "split (2-proc per-stream proxy)" C rate that EXCLUDES the
`loaded+interned` phase (load_corpus + Interner::process, i.e. the line interning that
produces the region ids the encoder consumes).  That phase is encoder work, so this
script reports both:
  Cgbps_codec = the published proxy (what the p29-bsc ledger reports)
  Cgbps_full  = raw / (process wall - decode share), i.e. all C-side work
"""
import glob
import os
import re
import statistics
import sys

RUNS = os.path.expanduser("~/selbind/runs")
CORPORA = os.path.expanduser("~/grouprlz/corpora.tsv")
ORDER = ["corpus", "corpus2", "corpus3", "corpus4", "corpus5", "corpus6", "corpus7",
         "corpus8", "corpus9", "corpus10", "corpus11", "corpus12", "corpus13",
         "corpus14", "corpus15", "corpus16"]
PRETTY = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus4": "abseil",
          "corpus5": "opencv", "corpus6": "godot", "corpus7": "fmt", "corpus8": "spdlog",
          "corpus9": "catch2", "corpus10": "nlohmann-json", "corpus11": "range-v3",
          "corpus12": "eigen", "corpus13": "re2", "corpus14": "leveldb",
          "corpus15": "simdjson", "corpus16": "cereal"}

C_FLOOR = 1.0      # GB/s, C-encode
F_FLOOR = 0.5      # GB/s, F-decode single thread


def read(path):
    with open(path) as handle:
        return handle.read()


def grab(pattern, text, group=1, cast=float):
    match = re.search(pattern, text)
    return cast(match.group(group)) if match else None


def series(pattern, paths, cast=float):
    out = []
    for path in sorted(paths):
        value = grab(pattern, read(path), cast=cast)
        if value is not None:
            out.append(value)
    return out


def med(values):
    return statistics.median(values) if values else None


def fmt(value, digits=3):
    return "NA" if value is None else f"{value:.{digits}f}"


def load_reference():
    ref = {}
    for line in read(CORPORA).splitlines()[1:]:
        f = line.split("\t")
        ref[f[0]] = dict(name=f[1], tus=int(f[2]), raw=int(f[3]), wp_z19=int(f[4]))
    return ref


def cell(cid, ref):
    d = os.path.join(RUNS, cid)
    raw = ref[cid]["raw"]
    row = dict(id=cid, name=PRETTY.get(cid, cid), tus=ref[cid]["tus"], raw=raw,
               wp_z19=ref[cid]["wp_z19"])

    # ---------------- GRZ2
    enc8 = sorted(glob.glob(f"{d}/grz.j8.enc.*.out"))
    row["GRZ2_bytes"] = int(read(enc8[0]).split("\t")[1])
    walls8 = [float(read(p)) for p in sorted(glob.glob(f"{d}/grz.j8.enc.*.wall"))]
    walls1 = [float(read(p)) for p in sorted(glob.glob(f"{d}/grz.j1.enc.*.wall"))]
    row["GRZ2_Cgbps_j8"] = med([raw / 1e9 / w for w in walls8])
    row["GRZ2_Cgbps_j8_min"] = min([raw / 1e9 / w for w in walls8])
    row["GRZ2_Cgbps_1T"] = med([raw / 1e9 / w for w in walls1])
    codec_c = series(r"C=(\d+) B/s", glob.glob(f"{d}/grz.j8.enc.*.err"))
    row["GRZ2_Cgbps_j8_codec"] = med([c / 1e9 for c in codec_c])
    codec_f = series(r"F=(\d+) B/s", glob.glob(f"{d}/grz.dec.*.err"))
    row["GRZ2_Fgbps_1T"] = med([f / 1e9 for f in codec_f])
    row["GRZ2_exact"] = read(f"{d}/grz.exact").strip()
    sha8 = set(read(f"{d}/grz.j8.wiresha").split())
    sha1 = set(read(f"{d}/grz.j1.wiresha").split())
    row["GRZ2_wire_sha256"] = sorted(sha8)[0]
    row["GRZ2_wire_stable"] = "YES" if len(sha8) == 1 and sha8 == sha1 else "NO"

    # ---------------- P29 + BSC
    def p29(tag):
        outs = sorted(glob.glob(f"{d}/p29.{tag}.*.out"))
        errs = sorted(glob.glob(f"{d}/p29.{tag}.*.err"))
        walls = [float(read(p)) for p in sorted(glob.glob(f"{d}/p29.{tag}.*.wall"))]
        total = grab(r"TOTAL=(\d+)", read(outs[0]), cast=int)
        exact = all("byte-exact=OK" in read(p) for p in outs)
        cs = series(r"C-encode ([0-9.]+) GB/s", errs)
        fs = series(r"F-decode ([0-9.]+) GB/s", errs)
        intern = series(r"loaded\+interned ([0-9.]+)s", errs)
        # full C = whole process minus the decode share the proxy attributes to F
        full = [raw / 1e9 / (w - raw / 1e9 / f) for w, f in zip(walls, fs)]
        return dict(bytes=total, exact=exact, c=med(cs), f=med(fs), c_full=med(full),
                    c_full_min=min(full), intern=med(intern), wall=med(walls),
                    walls=walls, fs=fs)

    nat, one = p29("native"), p29("1T")
    assert nat["bytes"] == one["bytes"], f"{cid}: P29 wire differs by thread count"
    row["P29BSC_bytes"] = nat["bytes"]
    row["P29BSC_Cgbps_codec"] = nat["c"]          # published basis (no interning charge)
    row["P29BSC_Cgbps_full"] = nat["c_full"]      # all C-side work, 16 cores
    row["P29BSC_Cgbps_full_min"] = nat["c_full_min"]
    row["P29BSC_Cgbps_full_1T"] = one["c_full"]
    row["P29BSC_Fgbps_1T"] = one["f"]
    row["P29BSC_Fgbps_native"] = nat["f"]
    row["P29BSC_intern_s"] = nat["intern"]
    # The interner is serial, so its throughput is a hard ceiling on the whole C side.
    row["P29BSC_intern_gbps"] = raw / 1e9 / nat["intern"] if nat["intern"] else None
    row["P29BSC_wall_s"] = nat["wall"]
    # Most generous reading available to P29+BSC: also forgive the in-memory copy of the
    # corpus (measured separately as a warm-cache `cat` of the same manifest).
    read_s = float(read(f"{d}/read.warm.s"))
    row["P29BSC_Cgbps_full_noread"] = med(
        [raw / 1e9 / (w - raw / 1e9 / f - read_s) for w, f in zip(nat["walls"], nat["fs"])])
    # rate spread across reps: >10% means the box was not quiet enough to trust the row
    row["C_spread_pct"] = 100.0 * (max(walls8) - min(walls8)) / med(walls8)
    row["P29BSC_exact"] = "YES" if nat["exact"] and one["exact"] else "NO"
    return row


def select(row, c_key_p29, c_key_grz):
    """Causal selection, exactly as the owner specified it.

    P29+BSC is the always-on baseline: it always ships, so it is never dropped on rate
    grounds -- whether IT met the deadline is a property of the whole scheme, reported
    separately.  GRZ2 is the opportunistic size win, kept only when its own encode beat
    the raw/1e9 deadline (a straggler is killed at the deadline, so this is causal).
    """
    p29_legal = row[c_key_p29] is not None and row[c_key_p29] >= C_FLOOR
    grz_legal = row[c_key_grz] is not None and row[c_key_grz] >= C_FLOOR
    if grz_legal and row["GRZ2_bytes"] < row["P29BSC_bytes"]:
        return "GRZ2", row["GRZ2_bytes"], p29_legal, grz_legal
    return "P29BSC", row["P29BSC_bytes"], p29_legal, grz_legal


def main():
    ref = load_reference()
    done = [c for c in ORDER
            if len(glob.glob(f"{RUNS}/{c}/p29.1T.*.wall")) == 3
            and os.path.exists(f"{RUNS}/{c}/grz.exact")]
    missing = [c for c in ORDER if c not in done]
    if missing:
        print(f"# INCOMPLETE, not yet measured: {' '.join(missing)}", file=sys.stderr)
    rows = [cell(c, ref) for c in done]

    cols = ["id", "name", "tus", "raw", "wp_z19",
            "P29BSC_bytes", "P29BSC_Cgbps_full", "P29BSC_Cgbps_full_1T",
            "P29BSC_Cgbps_full_noread", "P29BSC_Cgbps_codec", "P29BSC_Fgbps_1T",
            "P29BSC_intern_s", "P29BSC_intern_gbps", "P29BSC_wall_s",
            "GRZ2_bytes", "GRZ2_Cgbps_j8", "GRZ2_Cgbps_1T", "GRZ2_Cgbps_j8_codec",
            "GRZ2_Fgbps_1T",
            "P29BSC_ratelegal", "GRZ2_ratelegal", "selected_codec", "selected_bytes",
            "selected_over_z19", "selected_Fgbps_1T", "selected_F_ok", "decode_exact",
            "P29BSC_ratelegal_pub", "GRZ2_ratelegal_pub", "selected_codec_pub",
            "selected_bytes_pub", "C_spread_pct", "GRZ2_wire_sha256", "GRZ2_wire_stable",
            "P29BSC_exact"]
    print("\t".join(cols))

    tot = dict(raw=0, z19=0, sel=0, sel_pub=0, grz=0, p29=0, best=0)
    wins, wins_pub = dict(GRZ2=0, P29BSC=0), dict(GRZ2=0, P29BSC=0)
    floor_miss, f_miss, lost = [], [], []
    for row in rows:
        codec, size, pl, gl = select(row, "P29BSC_Cgbps_full", "GRZ2_Cgbps_j8")
        codec_p, size_p, plp, glp = select(row, "P29BSC_Cgbps_codec", "GRZ2_Cgbps_j8_codec")
        sel_f = row[f"{codec}_Fgbps_1T"]
        row.update(P29BSC_ratelegal=pl, GRZ2_ratelegal=gl, selected_codec=codec,
                   selected_bytes=size, selected_over_z19=size / row["wp_z19"],
                   selected_Fgbps_1T=sel_f, selected_F_ok=sel_f >= F_FLOOR,
                   decode_exact=row[f"{codec}_exact"],
                   P29BSC_ratelegal_pub=plp, GRZ2_ratelegal_pub=glp,
                   selected_codec_pub=codec_p, selected_bytes_pub=size_p)
        print("\t".join(
            fmt(row[c]) if isinstance(row[c], float) else str(row[c]) for c in cols))

        tot["raw"] += row["raw"]
        tot["z19"] += row["wp_z19"]
        tot["grz"] += row["GRZ2_bytes"]
        tot["p29"] += row["P29BSC_bytes"]
        tot["best"] += min(row["GRZ2_bytes"], row["P29BSC_bytes"])
        tot["sel"] += size
        tot["sel_pub"] += size_p
        wins[codec] += 1
        wins_pub[codec_p] += 1
        if not pl:
            floor_miss.append((row["name"], row["P29BSC_Cgbps_full"],
                               row["P29BSC_Cgbps_codec"]))
        if sel_f < F_FLOOR:
            f_miss.append((row["name"], codec, sel_f))
        if (row["GRZ2_bytes"] < row["P29BSC_bytes"]) and not gl:
            lost.append((row["name"], row["P29BSC_bytes"] - row["GRZ2_bytes"],
                         row["GRZ2_Cgbps_j8"]))

    def block(label, total, win):
        print(f"# {label}: bytes={total}  over_z19={total / tot['z19']:.4f}  "
              f"x_raw={tot['raw'] / total:.2f}  wins={win}", file=sys.stderr)

    # A third candidate: the same GRZ2 under size-keyed encode parameters -- bigger wire,
    # but fast enough to survive the deadline where the frozen 8 MiB policy misses it.
    tuned_path = os.path.expanduser("~/selbind/tuned-grz2.tsv")
    tuned = {}
    if os.path.exists(tuned_path):
        for line in read(tuned_path).splitlines():
            if line.startswith("#") or not line.strip():
                continue
            f = line.split("\t")
            tuned[f[0]] = dict(bytes=int(f[1]), c=float(f[2]), c_min=float(f[3]),
                               c_wall=float(f[4]), f=float(f[5]), policy=f[6])

    def variant(label, c_key, need_f, use_tuned=True):
        """Selector over {P29+BSC always-on} + {frozen GRZ2} + {tuned GRZ2}."""
        total, win = 0, dict(P29BSC=0, GRZ2=0, GRZ2_tuned=0)
        for row in rows:
            best, who = row["P29BSC_bytes"], "P29BSC"
            if (row["GRZ2_Cgbps_j8_codec"] >= C_FLOOR
                    and (not need_f or row["GRZ2_Fgbps_1T"] >= F_FLOOR)
                    and row["GRZ2_bytes"] < best):
                best, who = row["GRZ2_bytes"], "GRZ2"
            hit = tuned.get(row["id"]) if use_tuned else None
            if (hit and hit[c_key] >= C_FLOOR and (not need_f or hit["f"] >= F_FLOOR)
                    and hit["bytes"] < best):
                best, who = hit["bytes"], "GRZ2_tuned"
            total += best
            win[who] += 1
        block(label, total, win)
        need = tot["raw"] / 400.0
        verdict = "CLEARS" if total <= need else "SHORT"
        print(f"#   400x needs <= {need:.0f} B -> {verdict} by {abs(total - need):.0f} B",
              file=sys.stderr)
        return total

    variant("frozen policy, C AND F>=500MB/s gates", "c", True, use_tuned=False)
    if tuned:
        variant("+tuned GRZ2, C gate only, codec timer (median)", "c", False)
        variant("+tuned GRZ2, C gate only, codec timer (min of reps)", "c_min", False)
        variant("+tuned GRZ2, C gate only, process wall clock", "c_wall", False)
        variant("+tuned GRZ2, C AND F>=500MB/s gates, codec timer", "c", True)

    big = [r for r in rows if r["tus"] >= 100]
    if len(big) != len(rows):
        braw = sum(r["raw"] for r in big)
        bz19 = sum(r["wp_z19"] for r in big)
        bsel = sum(r["selected_bytes"] for r in big)
        bbest = sum(min(r["GRZ2_bytes"], r["P29BSC_bytes"]) for r in big)
        print(f"# --- {len(big)} corpora with >= 100 TUs only ---", file=sys.stderr)
        print(f"#   causal   bytes={bsel} over_z19={bsel / bz19:.4f} x_raw={braw / bsel:.2f}",
              file=sys.stderr)
        print(f"#   hindsight bytes={bbest} over_z19={bbest / bz19:.4f} "
              f"x_raw={braw / bbest:.2f}", file=sys.stderr)

    print(f"# raw_total={tot['raw']}  wp_z19_total={tot['z19']}", file=sys.stderr)
    print(f"# GRZ2_only   bytes={tot['grz']}  over_z19={tot['grz'] / tot['z19']:.4f}  "
          f"x_raw={tot['raw'] / tot['grz']:.2f}", file=sys.stderr)
    print(f"# P29BSC_only bytes={tot['p29']}  over_z19={tot['p29'] / tot['z19']:.4f}  "
          f"x_raw={tot['raw'] / tot['p29']:.2f}", file=sys.stderr)
    print(f"# HINDSIGHT_min bytes={tot['best']}  over_z19={tot['best'] / tot['z19']:.4f}  "
          f"x_raw={tot['raw'] / tot['best']:.2f}", file=sys.stderr)
    block("CAUSAL selector (full C cost)", tot["sel"], wins)
    block("CAUSAL selector (published C basis)", tot["sel_pub"], wins_pub)
    print(f"# P29+BSC 1 GB/s FLOOR misses (full C cost): {len(floor_miss)}/{len(rows)}",
          file=sys.stderr)
    for name, full, pub in floor_miss:
        print(f"#   {name}: full C={full:.3f} GB/s (published proxy said {pub:.3f})",
              file=sys.stderr)
    print(f"# selected-codec F<500 MB/s single thread: {len(f_miss)}/{len(rows)}",
          file=sys.stderr)
    for name, codec, rate in f_miss:
        print(f"#   {name}: {codec} F={rate:.3f} GB/s", file=sys.stderr)
    for name, cost, rate in lost:
        print(f"# GRZ2 smaller but rate-illegal: {name} costs +{cost} B "
              f"(GRZ2 C={rate:.3f} GB/s)", file=sys.stderr)


if __name__ == "__main__":
    main()
