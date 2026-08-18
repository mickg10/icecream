#!/usr/bin/env python3
"""Verify + extract the authoritative ice-ii-corpus-v1 cells.

Payload is <cell>/ii.tar.zst declared by <cell>/corpus.json; manifest.tsv sits
BESIDE the archive (the tar contains only ii/). Each cell is checked against
local-oracle's frozen verified-44-cells.tsv (payload sha256, TU count, raw bytes).
"""
import csv, hashlib, json, pathlib, shutil, subprocess

M = pathlib.Path("/home/ttuser/ictmp/ii-matrix")
C = pathlib.Path("/home/ttuser/gdict/cells44")
C.mkdir(parents=True, exist_ok=True)

V = {}
for r in csv.DictReader(open("/home/ttuser/gdict/verified-44-cells.tsv"), delimiter="\t"):
    V[(r["project"], r["profile"])] = (int(r["tu_count"]), int(r["raw_bytes"]), r["payload_sha256"])


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def do(proj, prof):
    d = M / proj / prof
    cj = d / "corpus.json"
    if not cj.is_file():
        return f"{proj}/{prof}\tNO_CORPUS_JSON"
    pay = json.load(cj.open())["payload"]
    arc = d / pay["path"]
    if not arc.is_file():
        return f"{proj}/{prof}\tNO_PAYLOAD"
    a = sha(arc)
    if a != pay["sha256"]:
        return f"{proj}/{prof}\tCORPUS_JSON_SHA_MISMATCH"
    v = V.get((proj, prof))
    tag = "verified-44" if v else "v2-pool"
    if v and a != v[2]:
        return f"{proj}/{prof}\tV44_SHA_MISMATCH"
    out = C / f"{proj}__{prof}"
    if not (out / "ii").is_dir():
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        p1 = subprocess.Popen(["zstd", "-dc", "--long=31", str(arc)], stdout=subprocess.PIPE)
        subprocess.check_call(["tar", "-xf", "-", "-C", str(out)], stdin=p1.stdout)
        p1.wait()
    rows = list(csv.DictReader((d / "manifest.tsv").open(), delimiter="\t"))
    (C / f"{proj}__{prof}.man").write_text("".join(f"{out}/{r['relative_path']}\n" for r in rows))
    raw = sum(int(r["raw_bytes"]) for r in rows)
    ok = "OK" if (not v or (len(rows) == v[0] and raw == v[1])) else "TU_OR_RAW_MISMATCH"
    return f"{proj}/{prof}\t{tag}\ttus={len(rows)}\traw={raw}\t{ok}"


targets = sorted(V)
targets += [("abseil", f) for f in ("conan-gcc", "debian-gcc", "fedora-clang-libcxx", "linuxbrew")]
for p, f in targets:
    print(do(p, f), flush=True)
