#!/usr/bin/env python3
"""Overlay the pretrained vs empty per-TU learning curves.
Metric (per TU, same TU sequence): wire_bytes / raw_bytes = wire bytes spent per input
byte (log-Y). TU-1 gap = purest prior-transfer signal (empty knows nothing at TU1).
Reads ~/bakeoff/curves/<c>.tsv (pretrained) + ~/bakeoff/curves_empty/<c>.tsv (empty).
Emits a summary table + curve_overlay.json (per-TU arrays for plotting)."""
import glob, os, csv, json, statistics

BK = os.path.expanduser("~/bakeoff")
NAME = {"corpus":"LLVM","corpus2":"RocksDB","corpus3":"DuckDB","corpus4":"abseil-protobuf",
        "corpus5":"OpenCV","corpus6":"Godot","corpus7":"fmt","corpus8":"spdlog","corpus9":"Catch2",
        "corpus10":"nlohmann-json","corpus11":"range-v3","corpus12":"Eigen","corpus13":"re2",
        "corpus14":"LevelDB","corpus15":"simdjson","corpus16":"cereal","corpus17":"GCC",
        "corpus18":"Firefox","corpus19":"Qt6","corpus20":"ClickHouse","corpus21":"PyTorch",
        "corpus22":"Folly","corpus23":"Arrow","corpus24":"Bitcoin","corpus25":"V8"}
# prior trained on gcc-11/libstdc++ corpora; these 3 test on a different toolchain
MISMATCH = {"corpus18":"clang/libstdc++15","corpus20":"clang/libc++","corpus25":"clang23"}
PKG_LLVMG = {"corpus2","corpus5"}  # forced onto llvm-godot prior (leakage guard)

def load(path):
    out=[]
    with open(path) as f:
        r=csv.reader(f, delimiter="\t"); next(r)
        for row in r:
            if len(row) < 11: continue
            if 'online' not in row[0]: continue  # keep the online-learning spec; skip *-frozen rows (empty run emits both)
            out.append((int(row[1]), float(row[2]), float(row[7]), float(row[10])))  # tu, raw, wire, cum_ratio
    return out

rows=[]; overlay={}
for cp in sorted(glob.glob(f"{BK}/curves/*.tsv")):
    c=os.path.basename(cp)[:-4]
    ep=f"{BK}/curves_empty/{c}.tsv"
    if not os.path.exists(ep): continue
    P=load(cp); E=load(ep)
    if len(P)<1 or len(E)<1: continue
    n=min(len(P),len(E))
    p_wr=[w/r for (_,r,w,_) in P[:n]]
    e_wr=[w/r for (_,r,w,_) in E[:n]]
    tc = MISMATCH.get(c, "match")
    pkg = "llvm-godot" if c in PKG_LLVMG else "rocks-opencv"
    rows.append(dict(c=c, name=NAME.get(c,c), tc=("MISMATCH" if c in MISMATCH else "match"), pkg=pkg, n=n,
        e_tu1=e_wr[0], p_tu1=p_wr[0], tu1_save=e_wr[0]/p_wr[0],
        e_final=E[n-1][3], p_final=P[n-1][3], final_gain=P[n-1][3]/E[n-1][3],
        e_mean=statistics.mean(e_wr), p_mean=statistics.mean(p_wr)))
    overlay[c]=dict(name=NAME.get(c,c), tc=("MISMATCH" if c in MISMATCH else "match"),
                    tu=[t for (t,_,_,_) in P[:n]], pre=p_wr, empty=e_wr,
                    pre_cum=[cr for (_,_,_,cr) in P[:n]], empty_cum=[cr for (_,_,_,cr) in E[:n]])

rows.sort(key=lambda x:-x["tu1_save"])
print(f"{'project':<15}{'toolchain':<10}{'pkg':<13}{'TUs':>4}{'empty_TU1':>10}{'pre_TU1':>9}{'TU1_save':>9}{'emptyFin':>10}{'preFin':>9}{'finGain':>9}")
print("-"*98)
for x in rows:
    print(f"{x['name']:<15}{x['tc']:<10}{x['pkg']:<13}{x['n']:>4}{x['e_tu1']:>10.4f}{x['p_tu1']:>9.4f}{x['tu1_save']:>8.2f}x{x['e_final']:>9.0f}x{x['p_final']:>8.0f}x{x['final_gain']:>8.2f}x")
print("-"*98)
mm=[x for x in rows if x['tc']=='MISMATCH']; ma=[x for x in rows if x['tc']=='match']
print(f"matched (n={len(ma)}):  median TU1_save = {statistics.median([x['tu1_save'] for x in ma]):.2f}x   median finGain = {statistics.median([x['final_gain'] for x in ma]):.2f}x")
if mm: print(f"MISMATCH (n={len(mm)}): median TU1_save = {statistics.median([x['tu1_save'] for x in mm]):.2f}x   median finGain = {statistics.median([x['final_gain'] for x in mm]):.2f}x")
print("\nTU1_save = empty's TU1 wire/raw  /  pretrained's TU1 wire/raw = prior's cold-start advantage (>1 = prior helps)")
print("finGain  = pretrained cumulative ratio / empty cumulative ratio at the last TU (>1 = prior still ahead)")
json.dump(overlay, open("curve_overlay.json","w"))
print(f"\nwrote curve_overlay.json ({len(overlay)} projects)")
