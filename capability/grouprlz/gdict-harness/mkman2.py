#!/usr/bin/env python3
"""Build padded-prefix + test manifests from arbitrary cell manifests.

usage: mkman2.py OUT TEST_MAN [PREFIX_MAN ...]
The prefix is padded to a multiple of the BSC literal-group size by repeating
leading (already-interned, ~0-wire) TUs so no group straddles the boundary.
"""
import sys, pathlib
G = 112
OUT = pathlib.Path("/home/ttuser/gdict/manifests")

def load(p):
    return [l.strip() for l in pathlib.Path(p).read_text().splitlines() if l.strip()]

out, test_man, *prefix_mans = sys.argv[1:]
pre = [e for m in prefix_mans for e in load(m)]
if pre:
    n = len(pre)
    pre += [pre[i % n] for i in range((-n) % G)]   # cycle-pad: prior may be shorter than the pad
test = load(test_man)
OUT.mkdir(parents=True, exist_ok=True)
(OUT / out).write_text("".join(e + "\n" for e in pre + test))
print(f"{out}\tprefix_tus={len(pre)}\ttest_tus={len(test)}\ttotal={len(pre)+len(test)}\taligned={len(pre)%G==0}")
