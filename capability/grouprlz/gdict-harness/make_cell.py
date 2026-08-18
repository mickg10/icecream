#!/usr/bin/env python3
"""Package an ordered .ii TU list as an ice-ii-corpus-v1 cell.

Produces the exact shape local-oracle's run_selector_cell.sh consumes:
<out>/ii/NNNNNNNN.ii (hardlinked, no copy), <out>/manifest.tsv, <out>/ii.tar.zst,
<out>/corpus.json. The archive digest recorded in corpus.json is computed from the
file actually written, so prepare_selector_cell.py's payload check is meaningful.

usage: make_cell.py --name N --profile P --tu-manifest F --out-dir D
"""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys

CHUNK = 8 << 20


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(CHUNK), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--profile", required=True)
    ap.add_argument("--tu-manifest", required=True, type=pathlib.Path)
    ap.add_argument("--out-dir", required=True, type=pathlib.Path)
    a = ap.parse_args()

    srcs = [l.strip() for l in a.tu_manifest.read_text().splitlines() if l.strip()]
    if not srcs:
        sys.exit("empty TU manifest")

    out = a.out_dir.resolve()
    if (out / "corpus.json").is_file():
        print(f"{a.name}\tEXISTS\t{out}")
        return
    ii = out / "ii"
    if out.exists():
        shutil.rmtree(out)
    ii.mkdir(parents=True)

    rows = ["ordinal\trelative_path\traw_bytes\tsha256\toriginal_path\n"]
    total = 0
    for i, s in enumerate(srcs):
        rel = f"ii/{i:08d}.ii"
        dst = out / rel
        try:
            os.link(s, dst)           # hardlink: no second copy of tens of GB
        except OSError:
            shutil.copyfile(s, dst)
        size = dst.stat().st_size
        total += size
        rows.append(f"{i}\t{rel}\t{size}\t{sha256_file(dst)}\t{s}\n")
    (out / "manifest.tsv").write_text("".join(rows))

    arc = out / "ii.tar.zst"
    tar = subprocess.Popen(
        ["tar", "--sort=name", "--mtime=@0", "--owner=0", "--group=0",
         "--numeric-owner", "-cf", "-", "ii"],
        cwd=out, stdout=subprocess.PIPE)
    with open(arc, "wb") as f:
        subprocess.check_call(["zstd", "-3", "--long=31", "-T8", "-q", "-c"],
                              stdin=tar.stdout, stdout=f)
    tar.wait()

    (out / "corpus.json").write_text(json.dumps({
        "payload": {"bytes": arc.stat().st_size, "compression": "zstd-3 --long=31",
                    "format": "deterministic-tar", "path": "ii.tar.zst",
                    "sha256": sha256_file(arc)},
        "profile": a.profile, "project": a.name, "raw_bytes": total,
        "schema": "ice-ii-corpus-v1", "tu_count": len(srcs),
    }, indent=2, sort_keys=True) + "\n")
    print(f"{a.name}\tOK\ttus={len(srcs)}\traw={total}\tarchive={arc.stat().st_size}")


main()
