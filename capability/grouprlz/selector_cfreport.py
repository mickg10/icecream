#!/usr/bin/env python3
"""C->F-only wire under the binding 30-slot cold-shard constraint."""
import os

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus6": "godot",
      "corpus9": "catch2", "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
P = os.path.expanduser("~/selbind/cfsplit/cfsplit.tsv")
H = ("corpus tus shards wire cf_total fc_total carry cf_root cf_fill cf_control "
     "fc_need fc_control missing split_ok dir_ok").split()
IX = {k: i for i, k in enumerate(H)}
d = {}
for l in open(P).read().splitlines()[1:]:
    f = l.split("\t")
    d.setdefault(f[0], {})[int(f[IX["shards"]])] = {
        k: (int(f[IX[k]]) if k not in ("split_ok", "dir_ok") else f[IX[k]])
        for k in H[1:]}

order = sorted(d, key=lambda c: d[c][1]["wire"])
print("HOW MUCH OF THE WIRE IS C->F AT ALL  (the rest is Need + the F side's Acks)\n")
print("  %-9s %5s %14s %14s %8s %14s %8s"
      % ("corpus", "TUs", "wire k=1", "C->F k=1", "share", "C->F k=30", "share"))
for c in order:
    a, b = d[c][1], d[c][30]
    print("  %-9s %5d %14d %14d %7.2f%% %14d %7.2f%%"
          % (NM[c], a["tus"], a["wire"], a["cf_total"], a["cf_total"] / a["wire"] * 100,
             b["cf_total"], b["cf_total"] / b["wire"] * 100))

print("\nTHE BINDING CASE: one cold build sharded across 30 slots, C->F wire only\n")
print("  %-9s %14s %14s %8s %10s %10s %10s"
      % ("corpus", "C->F 1 slot", "C->F 30 slots", "x", "avoidable", "Fill share", "shard eff"))
av, ef = [], []
for c in order:
    a, b = d[c][1], d[c][30]
    avoid = (b["cf_total"] - a["cf_total"]) / b["cf_total"] * 100
    fill = b["cf_fill"] / b["cf_total"] * 100
    eff = b["cf_fill"] / 30 / a["cf_fill"]
    av.append(avoid)
    ef.append(eff)
    print("  %-9s %14d %14d %7.2fx %9.1f%% %9.1f%% %10.3f"
          % (NM[c], a["cf_total"], b["cf_total"], b["cf_total"] / a["cf_total"],
             avoid, fill, eff))
print("  %-9s %14s %14s %8s %9.1f%% %10s %10.3f"
      % ("MEAN", "", "", "", sum(av) / len(av), "", sum(ef) / len(ef)))

print("\nC->F COMPOSITION AT 30 SLOTS  (Root = references, Fill = definitions)\n")
print("  %-9s %14s %8s %14s %8s %12s %10s"
      % ("corpus", "cf_root", "share", "cf_fill", "share", "cf_control", "missing x"))
for c in order:
    a, b = d[c][30], d[c][1]
    print("  %-9s %14d %7.1f%% %14d %7.1f%% %12d %9.2fx"
          % (NM[c], a["cf_root"], a["cf_root"] / a["cf_total"] * 100,
             a["cf_fill"], a["cf_fill"] / a["cf_total"] * 100, a["cf_control"],
             a["missing"] / b["missing"]))

bad = [c for c in d for k in d[c] if not d[c][k]["split_ok"].startswith(
    d[c][k]["split_ok"].split("/")[1]) or not d[c][k]["dir_ok"].startswith(
    d[c][k]["dir_ok"].split("/")[1])]
print("\n  gate: split_ok and dir_ok on every row of every run -- %s"
      % ("ALL PASS" if not bad else "FAILURES: %s" % bad))
