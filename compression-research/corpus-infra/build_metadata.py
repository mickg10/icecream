#!/usr/bin/env python3
"""Phase 1c: fold the cheap metrics + the zstd sizes into per-corpus METADATA.json,
corpus-metrics.tsv and corpus-metrics.md."""
import json, os, subprocess, sys, datetime

INFRA = "/tanksmall/scratch/ictmp/corpus-infra"
ICT = "/tanksmall/scratch/ictmp"

CORPORA = ["corpus", "corpus2", "corpus3", "corpus4", "corpus5", "corpus6", "corpus7",
           "corpus8", "corpus9", "corpus10", "corpus11", "corpus12", "corpus13",
           "corpus14", "corpus15", "corpus16"]


def sh(cmd):
    # bash, not /bin/sh: lib_corpora.sh uses arrays and `source`.
    return subprocess.run(cmd, shell=True, executable="/bin/bash",
                          capture_output=True, text=True).stdout.strip()


def lib(fn, arg):
    return sh(f'source {INFRA}/lib_corpora.sh; {fn} {arg}')


def read_comp(key):
    p = os.path.join(INFRA, "comp", key)
    if not os.path.exists(p):
        return None
    v = open(p).read().strip()
    return None if v == "null" else int(v)


def mib(n):
    return "-" if n is None else f"{n / 1048576:.1f}"


def ratio(a, b):
    return "-" if not a or not b else f"{a / b:.2f}"


ZSTD_VER = sh("zstd --version | grep -o 'v[0-9.]*'") or sh("zstd --version")
ZSTD_VER = f"zstd {ZSTD_VER}"
NOW = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
WINDOW_NOTE = ("z19long = zstd -19 --long=31 (2 GiB LDM window); z19 = plain (8 MB)")

rows = []
for c in CORPORA:
    cheap = dict(l.split("\t") for l in open(f"{INFRA}/cheap/{c}.tsv").read().splitlines())
    srcs = lib("srcs_of", c).split()
    present = [s for s in srcs if os.path.isdir(s)]
    urls, shas = [], []
    for s in present:
        if os.path.isdir(os.path.join(s, ".git")):
            urls.append(sh(f"git -C {s} config --get remote.origin.url"))
            shas.append(sh(f"git -C {s} rev-parse --short HEAD"))
    have_src = bool(present) and int(cheap["nfiles_src"]) > 0

    m = {
        "corpus_id": c,
        "project": lib("proj_of", c),
        "description": lib("desc_of", c),
        "source_checkouts": srcs,
        "git_url": " ".join(urls) if urls else None,
        "git_commit": " ".join(shas) if shas else None,
        "TU": int(cheap["TU"]),
        "loc_src": int(cheap["loc_src"]) if have_src else None,
        "bytes_src": int(cheap["bytes_src"]) if have_src else None,
        "nfiles_src": int(cheap["nfiles_src"]) if have_src else None,
        "loc_ii": int(cheap["loc_ii"]),
        "bytes_ii": int(cheap["bytes_ii"]),
        "z19_src": read_comp(f"{c}.z19_src"),
        "z19long_src": read_comp(f"{c}.z19long_src"),
        "z19_ii": read_comp(f"{c}.z19_ii"),
        "z19long_ii": read_comp(f"{c}.z19long_ii"),
        "zstd_version": ZSTD_VER,
        "window_note": WINDOW_NOTE,
        "build_recipe": lib("build_recipe_of", c),
        "generated_utc": NOW,
    }
    if not have_src:
        m["note"] = "source_unavailable"
    if c == "corpus":
        # The checkout is the whole llvm-project monorepo but only llvm/ was
        # preprocessed, so carry a second, fairer source basis alongside.
        ll = f"{INFRA}/lists/corpus.src-llvmonly.list"
        if os.path.exists(ll):
            files = open(ll).read().split()
            m["source_subset_llvm_only"] = {
                "note": "llvm/ subtree only -- the part that was actually preprocessed",
                "nfiles_src": len(files),
                "bytes_src": sum(os.path.getsize(f) for f in files),
                "z19_src": read_comp("corpus.z19_src_llvmonly"),
                "z19long_src": read_comp("corpus.z19long_src_llvmonly"),
            }
    snap = f"{INFRA}/snapshots/{c}.tar.zst"
    if os.path.exists(snap):
        m["snapshot_bytes"] = os.path.getsize(snap)
        m["snapshot_sha256"] = sh(f"sha256sum {snap}").split()[0]
    with open(f"{ICT}/{c}/METADATA.json", "w") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    rows.append(m)

# ------------------------------------------------------------------ TSV
cols = ["corpus", "project", "TU", "LOC_src", "LOC_ii", "bytes_src", "bytes_ii",
        "z19_src", "z19long_src", "z19_ii", "z19long_ii"]
have_snap = any("snapshot_bytes" in r for r in rows)
if have_snap:
    cols.append("snapshot_bytes")
with open(f"{INFRA}/corpus-metrics.tsv", "w") as f:
    f.write("\t".join(cols) + "\n")
    for r in rows:
        vals = [r["corpus_id"], r["project"], r["TU"], r["loc_src"], r["loc_ii"],
                r["bytes_src"], r["bytes_ii"], r["z19_src"], r["z19long_src"],
                r["z19_ii"], r["z19long_ii"]]
        if have_snap:
            vals.append(r.get("snapshot_bytes"))
        f.write("\t".join("" if v is None else str(v) for v in vals) + "\n")

# ------------------------------------------------------------------- MD
tot = {k: sum(r[k] or 0 for r in rows) for k in
       ("TU", "loc_src", "loc_ii", "bytes_src", "bytes_ii",
        "z19_src", "z19long_src", "z19_ii", "z19long_ii")}
tot["snapshot_bytes"] = sum(r.get("snapshot_bytes") or 0 for r in rows)

HDR = ("| corpus | project | TU | LOC src | LOC .ii | src MiB | .ii MiB | "
       "z19 src MiB | z19long src MiB | z19 .ii MiB | z19long .ii MiB | "
       "ii/src bytes | z19long ii/src | long gain (z19/z19long .ii) |")
SEP = "|" + "---|" * 14

lines = [HDR, SEP]
for r in rows:
    lines.append("| " + " | ".join([
        r["corpus_id"], r["project"], f"{r['TU']:,}",
        f"{r['loc_src']:,}" if r["loc_src"] else "-",
        f"{r['loc_ii']:,}",
        mib(r["bytes_src"]), mib(r["bytes_ii"]),
        mib(r["z19_src"]), mib(r["z19long_src"]),
        mib(r["z19_ii"]), mib(r["z19long_ii"]),
        ratio(r["bytes_ii"], r["bytes_src"]),
        ratio(r["z19long_ii"], r["z19long_src"]),
        ratio(r["z19_ii"], r["z19long_ii"]),
    ]) + " |")
lines.append("| **TOTAL** | 16 projects | " + " | ".join([
    f"{tot['TU']:,}", f"{tot['loc_src']:,}", f"{tot['loc_ii']:,}",
    mib(tot["bytes_src"]), mib(tot["bytes_ii"]),
    mib(tot["z19_src"]), mib(tot["z19long_src"]),
    mib(tot["z19_ii"]), mib(tot["z19long_ii"]),
    ratio(tot["bytes_ii"], tot["bytes_src"]),
    ratio(tot["z19long_ii"], tot["z19long_src"]),
    ratio(tot["z19_ii"], tot["z19long_ii"]),
]) + " |")
TABLE = "\n".join(lines)

snap_tbl = ""
if have_snap:
    sl = ["| corpus | snapshot | bytes | MiB | vs z19long .ii |", "|---|---|---|---|---|"]
    for r in rows:
        b = r.get("snapshot_bytes")
        sl.append(f"| {r['corpus_id']} | {r['corpus_id']}.tar.zst | "
                  f"{b:,} | {mib(b)} | {ratio(b, r['z19long_ii'])} |")
    sl.append(f"| **TOTAL** | | {tot['snapshot_bytes']:,} | {mib(tot['snapshot_bytes'])} | "
              f"{ratio(tot['snapshot_bytes'], tot['z19long_ii'])} |")
    snap_tbl = "\n## Snapshots\n\n" + "\n".join(sl) + """

Each snapshot is `tar` of that corpus's `.ii` files (relative paths) plus
`manifest.txt` and `METADATA.json`, compressed with `zstd -19 --long=31 -T0`.
Snapshot bytes run slightly above `z19long .ii` because of tar's 512-byte
per-member headers and padding (thousands of members per corpus) plus the extra
metadata files. Checksums: `snapshots/SHA256SUMS`.

**Unpacking requires `--long=31`.** zstd refuses a 2 GiB decompression window unless
asked, so a plain `zstd -d` fails with *"Frame requires too much memory for decoding"*.
Use:

```bash
zstd -dc --long=31 snapshots/corpusN.tar.zst | tar -x -C <dest>
```

The `METADATA.json` *inside* an archive is the copy that existed when the archive was
built, so it has no `snapshot_bytes`/`snapshot_sha256` keys — an archive cannot carry
its own size. The live `corpusN/METADATA.json` does.
"""

DOC = f"""# Corpus metrics: preprocessed (.ii) vs raw source, z19 and z19+LDM

Generated {NOW} with `{ZSTD_VER}` on a 12-core Xeon Gold 6136.
Source data: `corpus-metrics.tsv`; per-corpus `METADATA.json` lives in each
`/tanksmall/scratch/ictmp/corpusN/`.

All byte counts are **logical** (`stat -c%s` / `wc -c` / `wc -l`). `/tanksmall` is a
compressed filesystem, so `du` under-reports and is never used here.

## Method

* `.ii` set = exactly the files listed in that corpus's `manifest.txt`.
* raw source set = every `*.{{c,cc,cpp,cxx,c++,C,h,hh,hpp,hxx,h++,tcc,inl,inc,ipp,ixx}}`
  under the project's own checkout(s), excluding `/.git/`, `/build/`, `/CMakeFiles/`
  and any `*.ii`. Vendored / checked-in `third_party` **is** included: it is real
  shipped source.
* Compressed sizes are over the **pure concatenated content** of the file set
  (`cat file1 file2 ... | zstd ... | wc -c`) — no tar, so no per-file header overhead.
* `z19` = `zstd -19 -T0` (8 MB window). `z19long` = `zstd -19 --long=31 -T0`
  (2 GiB long-distance-matching window, i.e. the whole corpus is in reach).

## The comparison this table does and does not make

**The raw-source column excludes system and toolchain headers** — libstdc++, glibc,
and the compiler's own intrinsic headers — which the `.ii` expand inline, over and
over, once per TU. So:

* `z19long src` = *"ship the project's source; the remote already has the toolchain."*
* `z19long .ii` = *"ship all preprocessed output."*

That is the meaningful icecream comparison — what a distributed-compile transport
actually has to move, versus what it would move if the far end could be trusted to
have an identical toolchain and only needed project source. It is **not** a
like-for-like compression benchmark of identical bytes: the two columns describe
different content. The `z19long ii/src` ratio is therefore a transport-cost ratio,
not a compression-efficiency ratio.

## Table

{TABLE}

### Reading the derived columns

* **ii/src bytes** — raw expansion factor of preprocessing. Header-only libraries
  (Catch2 346x, Eigen 227x, cereal 165x) blow up hardest: a few KB of test source
  pulls in megabytes of templates every time.
* **z19long ii/src** — the headline number: compressed preprocessed footprint over
  compressed project-source footprint. Under 1.0 means the compressed `.ii` stream is
  *smaller* than the compressed project source, which happens whenever the corpus is
  many near-identical expansions of a small amount of source.
* **long gain** — how much the 2 GiB LDM window buys over the default 8 MB one on
  the `.ii` side. This is the cross-TU redundancy that an 8 MB window cannot see, and
  it is the entire premise of a cross-TU line-dedup transport. On the raw-source side
  the same window buys almost nothing — 1.00x for ten of the sixteen, 1.13x at the very
  most (simdjson, which checks in generated amalgamations) — because ordinary source
  has no cross-file duplication at that scale. That gap between the two columns *is*
  the opportunity.

### LLVM footnote: monorepo vs `llvm/` only

`corpus` is the one row where the source column is badly mismatched to the `.ii`
column. The checkout is the whole llvm-project monorepo — clang, mlir, libcxx, lldb,
all the test inputs — but only `llvm/` for the X86 target was preprocessed. Restricting
the source set to the `llvm/` subtree that was actually compiled:

| basis | files | src MiB | z19 src MiB | z19long src MiB | z19long ii/src |
|---|---|---|---|---|---|
| whole monorepo (table above) | {{LLVM_MONO}} |
| `llvm/` subtree only | {{LLVM_ONLY}} |

The `llvm/`-only basis is the honest one for this corpus, and it is the row to quote:
`{{LLVM_ONLY_RATIO}}`. Even so, LLVM stays the least redundant corpus in the set by this
measure: 1,238 TUs of genuinely different compiler source, not 1,238 re-expansions of the
same headers. The other 15 corpora do not have this problem — their checkouts are the
project that was preprocessed.
{snap_tbl}
## Caveats

* **corpus (LLVM)** — the source column covers the *entire* llvm-project monorepo
  (clang, mlir, libcxx, all of `llvm/test/`, ...), because that is what the checkout
  contains, while only `llvm/` for the X86 target was preprocessed. Its `ii/src` and
  `z19long ii/src` ratios are therefore pessimistic by a large factor; see the LLVM
  footnote above for `llvm/`-subtree-only numbers. The **TOTAL** row inherits the same
  distortion — its 0.58 would be ~0.75 on the `llvm/`-only basis — so read the per-corpus
  rows, not the total, when the source column matters.
* **corpus (LLVM)** TU count comes from a *time-bounded* build harvest
  (`-save-temps=obj`, 720 s at -j12), not from a full replay of a compile database,
  so it is only approximately reproducible.
* **corpus12 (Eigen)** is capped at 650 of 1542 available TUs; Eigen's ~5.4 MB per-TU
  expansion would otherwise make it ~8 GB on its own.
* **corpus15 (simdjson)** is 153 of 156 TUs: 3 developer-mode targets fail to
  preprocess and are dropped.
* **corpus4** is two projects (abseil + protobuf) sharing one manifest; its source
  column sums both checkouts, and `git_url`/`git_commit` carry both.
* Compressed sizes come from multi-threaded zstd (`-T0`, 12 workers). MT output can
  differ by a fraction of a percent from single-threaded output at the same level;
  every number here was produced the same way, so they are comparable to each other.

## Regenerating

`./regenerate_corpuses.sh [corpusN ...]` rebuilds corpora from scratch
(clone -> configure -> `-E` replay -> manifest -> TU verify); `./snapshot_corpuses.sh`
rebuilds the archives; `collect_cheap.sh` + `collect_zstd.sh` + `build_metadata.py`
rebuild this table. See `README.md`.
"""
llvm = rows[0]
sub = llvm.get("source_subset_llvm_only")
if sub:
    DOC = (DOC
           .replace("{LLVM_MONO}", " | ".join([
               f"{llvm['nfiles_src']:,}", mib(llvm["bytes_src"]),
               mib(llvm["z19_src"]), mib(llvm["z19long_src"]),
               ratio(llvm["z19long_ii"], llvm["z19long_src"])]))
           .replace("{LLVM_ONLY}", " | ".join([
               f"{sub['nfiles_src']:,}", mib(sub["bytes_src"]),
               mib(sub["z19_src"]), mib(sub["z19long_src"]),
               ratio(llvm["z19long_ii"], sub["z19long_src"])]))
           .replace("{LLVM_ONLY_RATIO}",
                    f"z19long ii/src = {ratio(llvm['z19long_ii'], sub['z19long_src'])}, "
                    f"not {ratio(llvm['z19long_ii'], llvm['z19long_src'])}"))

open(f"{INFRA}/corpus-metrics.md", "w").write(DOC)
print(TABLE)
print(f"\nwrote {INFRA}/corpus-metrics.tsv, {INFRA}/corpus-metrics.md and 16 METADATA.json")
