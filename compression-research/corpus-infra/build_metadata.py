#!/usr/bin/env python3
"""Fold the cheap metrics + the z19-long measurements into per-corpus METADATA.json,
a consolidated corpus-catalog.json, corpus-metrics.tsv and corpus-metrics.md.

Only `zstd -19 --long=31` is measured -- the plain 8 MB-window numbers were dropped.
"""
import json, os, subprocess, datetime

INFRA = "/tanksmall/scratch/ictmp/corpus-infra"
ICT = "/tanksmall/scratch/ictmp"

CORPORA = ["corpus", "corpus2", "corpus3", "corpus4", "corpus5", "corpus6", "corpus7",
           "corpus8", "corpus9", "corpus10", "corpus11", "corpus12", "corpus13",
           "corpus14", "corpus15", "corpus16"]

# zstd's largest window is --long=31 = 2 GiB.  A corpus whose .ii exceed that cannot
# have its whole content in reach at once, so its z19long_ii is a LOWER bound on the
# cross-TU redundancy actually present.
WINDOW_BYTES = 2 << 30


def sh(cmd):
    # bash, not /bin/sh: lib_corpora.sh uses arrays and `source`.
    return subprocess.run(cmd, shell=True, executable="/bin/bash",
                          capture_output=True, text=True).stdout.strip()


def lib(fn, arg):
    return sh(f'source {INFRA}/lib_corpora.sh; {fn} {arg}')


def read_comp(name):
    p = os.path.join(INFRA, "comp2", name)
    if not os.path.exists(p):
        return None
    v = open(p).read().strip()
    return None if v == "null" else int(v)


def mib(n):
    return "-" if n is None else f"{n / 1048576:.1f}"


def ratio(a, b):
    return "-" if not a or not b else f"{a / b:.2f}"


ZSTD_VER = "zstd " + (sh("zstd --version | grep -o 'v[0-9.]*'") or "?")
NOW = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
WINDOW_NOTE = ("z19long = zstd -19 --long=31, single-threaded (2 GiB LDM window -- "
               "zstd's maximum)")

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
    bytes_ii = int(cheap["bytes_ii"])

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
        "bytes_ii": bytes_ii,
        "z19long_src": read_comp(f"{c}.src"),
        "z19long_ii": read_comp(f"{c}.ii"),
        "window_limited": bytes_ii > WINDOW_BYTES,
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
                "z19long_src": read_comp("corpus.src_llvmonly"),
            }
    snap = f"{ICT}/corpus-snapshots/{c}.ii.tar.zst"
    if os.path.exists(snap):
        m["transfer_snapshot"] = {
            "file": f"{c}.ii.tar.zst",
            "bytes": os.path.getsize(snap),
            "codec": "zstd -3 --long=27 -T0 (transfer archive, not a measurement)",
        }
    with open(f"{ICT}/{c}/METADATA.json", "w") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    rows.append(m)

# ------------------------------------------------------- consolidated catalog
catalog = {
    "generated_utc": NOW,
    "zstd_version": ZSTD_VER,
    "window_note": WINDOW_NOTE,
    "measurement": ("compressed sizes are `cat <file set> | zstd -19 --long=31 | wc -c` "
                    "-- pure concatenated content, no tar framing, nothing stored"),
    "source_file_extensions": ["c", "cc", "cpp", "cxx", "c++", "C", "h", "hh", "hpp",
                               "hxx", "h++", "tcc", "inl", "inc", "ipp", "ixx"],
    "source_exclusions": ["/.git/", "/build/", "/CMakeFiles/", "*.ii"],
    "toolchain": sh("gcc --version | head -1"),
    "corpora": rows,
}
os.makedirs(f"{INFRA}/out", exist_ok=True)
with open(f"{INFRA}/out/corpus-catalog.json", "w") as f:
    json.dump(catalog, f, indent=2)
    f.write("\n")

# ------------------------------------------------------------------ TSV
cols = ["corpus", "project", "TU", "LOC_src", "LOC_ii", "bytes_src", "bytes_ii",
        "z19long_src", "z19long_ii", "z19long_ii_over_src"]
with open(f"{INFRA}/out/corpus-metrics.tsv", "w") as f:
    f.write("\t".join(cols) + "\n")
    for r in rows:
        rr = (r["z19long_ii"] / r["z19long_src"]
              if r["z19long_ii"] and r["z19long_src"] else None)
        vals = [r["corpus_id"], r["project"], r["TU"], r["loc_src"], r["loc_ii"],
                r["bytes_src"], r["bytes_ii"], r["z19long_src"], r["z19long_ii"],
                "" if rr is None else f"{rr:.4f}"]
        f.write("\t".join("" if v is None else str(v) for v in vals) + "\n")

# ------------------------------------------------------------------- MD
tot = {k: sum(r[k] or 0 for r in rows) for k in
       ("TU", "loc_src", "loc_ii", "bytes_src", "bytes_ii", "z19long_src", "z19long_ii")}

HDR = ("| corpus | project | TU | LOC src | LOC .ii | src MiB | .ii MiB | "
       "z19long src MiB | z19long .ii MiB | z19long ii/src |")
SEP = "|" + "---|" * 10

lines = [HDR, SEP]
for r in rows:
    dagger = " †" if r["window_limited"] else ""
    lines.append("| " + " | ".join([
        r["corpus_id"] + dagger, r["project"], f"{r['TU']:,}",
        f"{r['loc_src']:,}" if r["loc_src"] else "-",
        f"{r['loc_ii']:,}",
        mib(r["bytes_src"]), mib(r["bytes_ii"]),
        mib(r["z19long_src"]), mib(r["z19long_ii"]),
        ratio(r["z19long_ii"], r["z19long_src"]),
    ]) + " |")
lines.append("| **TOTAL** | 16 projects | " + " | ".join([
    f"{tot['TU']:,}", f"{tot['loc_src']:,}", f"{tot['loc_ii']:,}",
    mib(tot["bytes_src"]), mib(tot["bytes_ii"]),
    mib(tot["z19long_src"]), mib(tot["z19long_ii"]),
    ratio(tot["z19long_ii"], tot["z19long_src"]),
]) + " |")
TABLE = "\n".join(lines)

n_limited = sum(1 for r in rows if r["window_limited"])
limited_names = ", ".join(f"{r['corpus_id']} ({r['project']})"
                          for r in rows if r["window_limited"])

DOC = f"""# Corpus metrics: preprocessed (.ii) vs raw source at zstd -19 --long=31

Generated {NOW} with `{ZSTD_VER}` on a 12-core Xeon Gold 6136.
Machine-readable: `../data/corpus-metrics.tsv` and `../data/corpus-catalog.json`;
per-corpus `METADATA.json` lives in each `corpusN/` alongside its `manifest.txt`.

All byte counts are **logical** (`stat -c%s` / `wc -c` / `wc -l`). `/tanksmall` is a
compressed filesystem, so `du` under-reports and is never used here.

## Method

* `.ii` set = exactly the files listed in that corpus's `manifest.txt`.
* raw source set = every `*.{{c,cc,cpp,cxx,c++,C,h,hh,hpp,hxx,h++,tcc,inl,inc,ipp,ixx}}`
  under the project's own checkout(s), excluding `/.git/`, `/build/`, `/CMakeFiles/`
  and any `*.ii`. Vendored / checked-in `third_party` **is** included: it is real
  shipped source.
* Compressed sizes are over the **pure concatenated content** of the file set
  (`cat file1 file2 ... | zstd -19 --long=31 | wc -c`) — no tar, so no per-file header
  overhead, and nothing is written to disk. Single-threaded, so the numbers are
  reproducible (multi-threaded zstd changes framing and shifts sizes slightly).

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

**†** — `--long=31` (2 GiB) is zstd's **maximum** window; 32 is rejected even with
`--ultra`. The {n_limited} daggered corpora have more than 2 GiB of `.ii`, so the
window cannot span the whole corpus and zstd never gets to see the duplication between
the earliest and latest TUs. Their `z19long .ii` is therefore a **lower bound** on the
cross-TU redundancy present — the true figure is better than shown, and a codec with
its own unbounded line dictionary is not subject to this ceiling at all. Affected:
{limited_names}.

### Reading the table

* **ii/src bytes** (`.ii MiB` ÷ `src MiB`) — raw expansion factor of preprocessing.
  Header-only libraries blow up hardest: Catch2 346x, Eigen 227x, range-v3 178x,
  cereal 165x. A few KB of test source pulls in megabytes of templates every time.
* **z19long ii/src** — the headline number: compressed preprocessed footprint over
  compressed project-source footprint. Under 1.0 means the compressed `.ii` stream is
  *smaller* than the compressed project source, which happens whenever the corpus is
  many near-identical expansions of a small amount of source.

### LLVM footnote: monorepo vs `llvm/` only

`corpus` is the one row where the source column is badly mismatched to the `.ii`
column. The checkout is the whole llvm-project monorepo — clang, mlir, libcxx, lldb,
all the test inputs — but only `llvm/` for the X86 target was preprocessed. Restricting
the source set to the `llvm/` subtree that was actually compiled:

| basis | files | src MiB | z19long src MiB | z19long ii/src |
|---|---|---|---|---|
| whole monorepo (table above) | {{LLVM_MONO}} |
| `llvm/` subtree only | {{LLVM_ONLY}} |

The `llvm/`-only basis is the honest one for this corpus, and it is the row to quote:
`{{LLVM_ONLY_RATIO}}`. The other 15 corpora do not have this problem — their checkouts
are the project that was preprocessed. The **TOTAL** row inherits the same distortion,
so read the per-corpus rows, not the total, when the source column matters.

## Caveats

* **corpus (LLVM)** TU count comes from a *time-bounded* build harvest
  (`-save-temps=obj`, 720 s at -j12), not from a full replay of a compile database,
  so it is only approximately reproducible.
* **corpus12 (Eigen)** is capped at 650 of 1542 available TUs; Eigen's ~5.4 MB per-TU
  expansion would otherwise make it ~8 GB on its own.
* **corpus15 (simdjson)** is 153 of 156 TUs: 3 developer-mode targets fail to
  preprocess and are dropped.
* **corpus4** is two projects (abseil + protobuf) sharing one manifest; its source
  column sums both checkouts, and `git_url`/`git_commit` carry both.
* `.ii` size depends on where the checkout lives: `-E` emits a `# <line> "<abs path>"`
  marker at every include transition, so a longer source path inflates every corpus
  (measured: +1.3% for a path 31 characters longer, with the `.ii` byte-identical once
  both roots are normalized).

## Regenerating

`corpus-infra/regenerate_corpuses.sh [corpusN ...]` rebuilds corpora from scratch
(clone -> configure -> `-E` replay -> manifest -> TU verify);
`corpus-infra/snapshot_corpuses.sh` rebuilds the transfer archives;
`collect_cheap.sh` + `collect_zstd.sh` + `build_metadata.py` rebuild this table.
See `corpus-infra/README.md`.
"""

llvm = rows[0]
sub = llvm.get("source_subset_llvm_only")
if sub and sub.get("z19long_src"):
    DOC = (DOC
           .replace("{LLVM_MONO}", " | ".join([
               f"{llvm['nfiles_src']:,}", mib(llvm["bytes_src"]),
               mib(llvm["z19long_src"]),
               ratio(llvm["z19long_ii"], llvm["z19long_src"])]))
           .replace("{LLVM_ONLY}", " | ".join([
               f"{sub['nfiles_src']:,}", mib(sub["bytes_src"]),
               mib(sub["z19long_src"]),
               ratio(llvm["z19long_ii"], sub["z19long_src"])]))
           .replace("{LLVM_ONLY_RATIO}",
                    f"z19long ii/src = {ratio(llvm['z19long_ii'], sub['z19long_src'])}, "
                    f"not {ratio(llvm['z19long_ii'], llvm['z19long_src'])}"))

open(f"{INFRA}/out/corpus-metrics.md", "w").write(DOC)

# ------------------------------------------- compression-research/ index README
snap_total = 0
snapdir = f"{ICT}/corpus-snapshots"
if os.path.isdir(snapdir):
    snap_total = sum(os.path.getsize(os.path.join(snapdir, f))
                     for f in os.listdir(snapdir) if f.endswith(".tar.zst"))

cat_rows = "\n".join(
    f"| `{r['corpus_id']}` | {r['project']} | {r['TU']:,} | {mib(r['bytes_ii'])} | "
    f"{mib(r['z19long_ii'])} | {ratio(r['z19long_ii'], r['z19long_src'])} |"
    for r in rows)

INDEX = f"""# compression-research

Everything behind the icecream cross-TU compression work: the corpus set it is measured
on, the scripts that can rebuild that set from bare clones, the measurements, and the
reports written off them.

Generated {NOW}.

## What the corpora are

Sixteen C++ projects, each preprocessed into a corpus of `.ii` translation units — the
exact bytes a distributed-compile transport would have to move. **{tot['TU']:,} TUs,
{mib(tot['bytes_ii'])} MiB logical.**

| corpus | project | TU | .ii MiB | z19long .ii MiB | z19long ii/src |
|---|---|---|---|---|---|
{cat_rows}

The set is deliberately lopsided. Header-only template libraries (Catch2, Eigen,
range-v3, cereal) sit at one end, where hundreds of small TUs each re-expand the same
megabytes of headers; large application codebases (LLVM, Godot, OpenCV) sit at the
other, where each TU is mostly its own content. Cross-TU redundancy differs by two
orders of magnitude across that range, which is the point — a codec that only works on
the Catch2 end of the spectrum has not been tested.

Full numbers, method and caveats: **[reports/corpus-metrics.md](reports/corpus-metrics.md)**.
Machine-readable: [data/corpus-metrics.tsv](data/corpus-metrics.tsv) and
[data/corpus-catalog.json](data/corpus-catalog.json).

## The toolchain caveat

Read this before quoting any ratio. A `.ii` is the project's source *plus every system
and toolchain header it pulls in*, expanded inline, once per TU. The raw-source figures
cover only the project's own checkout and exclude libstdc++, glibc and compiler
intrinsics entirely. So the two compressed columns answer different questions:

* `z19long src` — *ship the project's source; the far end already has the toolchain.*
* `z19long .ii` — *ship all preprocessed output.*

That pair is the meaningful distributed-compile comparison. It is **not** a
like-for-like compression benchmark of identical bytes.

Two more things that bite: `--long=31` (2 GiB) is zstd's maximum window, so the six
corpora larger than that are window-limited and their `.ii` number is a *lower bound*
on the redundancy present; and the LLVM row's source column covers the whole monorepo
while only `llvm/` was preprocessed. Both are flagged in the metrics report.

## Layout

| | |
|---|---|
| `corpus-infra/` | rebuild the corpora (`regenerate_corpuses.sh` + `recipes/`), build the transfer archives (`snapshot_corpuses.sh`), and reproduce the metrics (`collect_cheap.sh`, `collect_zstd.sh`, `build_metadata.py`) |
| `data/` | the metrics table, the consolidated corpus catalog, and the study/leave-one-out datasets |
| `reports/` | the metrics report plus the design notes, analyses and published HTML reports |

Corpus data itself is **not** in the repo — the `.ii` files are {mib(tot['bytes_ii'])} MiB
and the transfer archives are gitignored.

## Running it

```bash
cd corpus-infra

# rebuild any corpus from a bare clone: clone -> configure -> -E replay -> manifest -> verify TU
./regenerate_corpuses.sh                     # all 16
./regenerate_corpuses.sh corpus7 corpus9     # some
./regenerate_corpuses.sh --force corpus7     # redo configure + preprocess

# reproduce the metrics table
./collect_cheap.sh && ./collect_zstd.sh -P 6 && python3 build_metadata.py

# build the transfer archives (zstd -3 --long=27) for shipping to another box
./snapshot_corpuses.sh -P 5
```

Moving the corpora to another machine — transfer, checksum, unpack, and the manifest
path rewrite that is easy to forget — is documented in
[corpus-infra/REHYDRATE.md](corpus-infra/REHYDRATE.md).
Requirements: git, cmake + ninja, GCC, python3, zstd, and `scons` for corpus6 only.
"""
open(f"{INFRA}/out/README.md", "w").write(INDEX)

print(TABLE)
print(f"\nwrote out/corpus-metrics.tsv, out/corpus-metrics.md, out/corpus-catalog.json"
      f" and {len(rows)} METADATA.json")
