#!/usr/bin/env python3
"""Build padded prefix + test manifests for the generic-dict cold-start experiment."""
import sys, pathlib
G = 112
ROOT = pathlib.Path("/home/ttuser/ictmp")
OUT = pathlib.Path("/home/ttuser/gdict/manifests")

def load(name):
    p = ROOT / name / "manifest.txt"
    return [l.strip() for l in p.read_text().splitlines() if l.strip()]

def pad(entries):
    """Pad to a multiple of G by repeating leading entries (already-known TUs, ~0 wire)."""
    k = (-len(entries)) % G
    return entries + entries[:k]

def main():
    mode, out = sys.argv[1], sys.argv[2]
    prefix_names = sys.argv[3].split(",") if sys.argv[3] else []
    test_name = sys.argv[4]
    pre = []
    for n in prefix_names:
        pre += load(n)
    pre = pad(pre) if pre else []
    test = load(test_name)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / out).write_text("".join(e + "\n" for e in pre + test))
    print(f"{out}\tmode={mode}\tprefix_tus={len(pre)}\ttest_tus={len(test)}\ttotal={len(pre)+len(test)}\tprefix_aligned={len(pre)%G==0}")

main()
