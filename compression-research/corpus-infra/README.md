# corpus-infra — the 16 preprocessed-TU corpora

Sixteen C++ projects, each preprocessed into a corpus of `.ii` translation units, used
as the workload set for icecream cross-TU line-dedup / compression work. This directory
holds the metrics, the snapshots, and the scripts that can rebuild any corpus from a
bare clone.

Everything lives under `/tanksmall/scratch/ictmp/`; the corpora themselves are the
`corpusN/` directories beside this one, each with a `manifest.txt` (one absolute `.ii`
path per line — the authoritative file list) and a `METADATA.json`.

| corpus | project | what it is | TU |
|---|---|---|---|
| corpus | LLVM | compiler infrastructure, `llvm/` only, X86 target | 1,238 |
| corpus2 | RocksDB | embedded persistent key-value store, lib + all tests | 622 |
| corpus3 | DuckDB | in-process analytical SQL database, in-tree extensions | 689 |
| corpus4 | abseil + protobuf | Abseil common libraries + protobuf `src/` (two projects, one corpus) | 700 |
| corpus5 | OpenCV | computer vision, default module set, tests + perf tests | 1,506 |
| corpus6 | Godot | game engine, all C++ TUs after SCons codegen | 2,207 |
| corpus7 | fmt | string formatting library, tests on | 50 |
| corpus8 | spdlog | fast logging library, tests + examples | 34 |
| corpus9 | Catch2 | unit-test framework, dev build + examples + extra tests | 857 |
| corpus10 | nlohmann-json | single-header JSON library, test suite | 99 |
| corpus11 | range-v3 | range algorithms/views, tests + examples | 259 |
| corpus12 | Eigen | header-only linear algebra, test suite (capped at 650 TUs) | 650 |
| corpus13 | re2 | regular-expression engine, pinned pre-Abseil | 72 |
| corpus14 | LevelDB | key-value storage, tests + benchmarks | 72 |
| corpus15 | simdjson | SIMD JSON parser, developer mode | 153 |
| corpus16 | cereal | header-only serialization, tests + sandbox | 84 |

The set is deliberately lopsided: header-only template libraries (Catch2, Eigen,
range-v3, cereal) at one end, where hundreds of small TUs each expand the same
megabytes of headers, and large application codebases (LLVM, Godot, OpenCV) at the
other, where each TU is mostly its own content. Cross-TU redundancy differs by two
orders of magnitude across that range.

## The pipeline

```
clone (shallow)  ->  configure  ->  preprocess (-E)  ->  manifest  ->  verify TU count
```

**configure** produces a `compile_commands.json`. No real build is needed for most
projects — the compile database alone is enough, which is why 10 of the 16 corpora
cost minutes rather than hours. Three exceptions:

* **Godot** needs a *full* SCons build first: every TU includes generated `.gen.h`
  files (shaders, `gdvirtual`, version, certs) that do not exist until the build runs.
* **OpenCV** needs a partial ninja run over its generated-source targets
  (`opencl_kernels_*.cpp`, generated headers).
* **LLVM** is not `-E`-replayed at all. It is configured with `-save-temps=obj` so the
  real build drops a `.ii` next to every object, and a *time-bounded* build harvests
  whatever it produced. Its TU count is therefore a function of the time bound.

**preprocess** (`preprocess_corpus.py`) re-runs each compile command with `-c -o x.o`
swapped for `-E -o <mirrored>.ii`: same compiler, same flags, same include paths, so
the `.ii` is byte-identical to what the compiler would have fed its own front end.
Output paths mirror the object paths under the corpus root, so names never collide.

The compile database is filtered before the replay: vendored test frameworks
(`third_party/googletest`, `third_party/benchmark`) and anything CMake `FetchContent`ed
into `build/_deps` are dropped, so a corpus never silently ingests another project's
code. LevelDB, for instance, is cloned `--recursive` because it needs googletest to
configure, but googletest's own TUs are not part of corpus14.

## Toolchain caveat (matters for reading the metrics)

A `.ii` is the project's source *plus every system and toolchain header it pulls in*,
expanded inline, once per TU. The raw-source figures in `corpus-metrics.md` cover only
the project's own checkout (project + vendored `third_party`) and exclude libstdc++,
glibc, and compiler intrinsic headers entirely.

So the two compressed columns answer different questions:

* `z19long src` — *ship the project's source; the far end already has the toolchain.*
* `z19long .ii` — *ship all preprocessed output.*

That pair is the meaningful distributed-compile comparison. It is **not** a
like-for-like compression benchmark of identical bytes.

The corpora were all preprocessed with the same GCC 11.4.0 on the same machine.
Regenerating on a host with a different toolchain will produce different `.ii` content
and different sizes — the corpora are reproducible in *structure*, not byte-for-byte
across toolchains.

**`.ii` size depends on where the checkout lives.** `-E` output carries a `# <line>
"<absolute path>"` marker for every include transition, so moving the source tree to a
longer path inflates every corpus. The regenerate test measured this: rebuilding
corpus7 (fmt, same commit) under a path 31 characters longer produced 138,152,710
bytes against the live corpus's 136,350,082 — 1.3% larger, entirely line markers, with
the `.ii` byte-identical once both roots are normalized. Keep that in mind before
reading a size delta as a content change.

## Files here

| file | what |
|---|---|
| `corpus-metrics.tsv` | machine-readable metrics, one row per corpus |
| `corpus-metrics.md` | same table with MiB, derived ratios, and the caveats |
| `regenerate_corpuses.sh` | rebuild corpora from bare clones |
| `recipes/<corpus>.sh` | per-corpus clone URL / pin / configure flags / expected TU |
| `preprocess_corpus.py` | the `-E` replay driver (copy of `build2/preprocess_corpus.py`) |
| `snapshot_corpuses.sh` | build `snapshots/corpusN.tar.zst` + `SHA256SUMS` |
| `lib_corpora.sh` | shared corpus -> project / source-checkout / description tables |
| `collect_cheap.sh` | counts, logical bytes, LOC; writes the file lists |
| `collect_zstd.sh` | the expensive z19 / z19+LDM pass |
| `build_metadata.py` | folds both into `METADATA.json` + the TSV + the MD |
| `lists/` | the exact `.ii` and source file lists each measurement used |
| `cheap/`, `comp/` | intermediate per-corpus counts and compressed sizes (resume points) |
| `logs/` | configure / build / preprocess / metrics logs |
| `snapshots/` | `corpusN.tar.zst` + `SHA256SUMS` |
| `test-regen/test.log` | transcript of the tested regenerate run (fmt) |

`comp/` doubles as the resume state for `collect_zstd.sh`: one file per measurement,
written only when the whole pipeline exited 0. Delete a file to recompute just that
number; delete the directory to redo the 25-minute pass.

## Running it

```bash
# rebuild the metrics (cheap pass ~90 s, zstd pass ~25 min over ~27 GiB)
./collect_cheap.sh && ./collect_zstd.sh && python3 build_metadata.py

# snapshots
./snapshot_corpuses.sh              # all; skips ones already built
./snapshot_corpuses.sh corpus7      # just one

# unpacking a snapshot -- --long=31 is REQUIRED, see below
zstd -dc --long=31 snapshots/corpus7.tar.zst | tar -x -C /somewhere

# regenerate corpora from scratch (clones into ./sources, corpora into ./corpora)
./regenerate_corpuses.sh                     # all 16
./regenerate_corpuses.sh corpus7 corpus9     # some
./regenerate_corpuses.sh --force corpus7     # redo configure + preprocess
./regenerate_corpuses.sh --outroot /elsewhere --srcroot /elsewhere/src corpus7
```

`regenerate_corpuses.sh` is idempotent: it skips the clone if the checkout exists,
skips configure if a `compile_commands.json` is there, and skips the whole corpus if
its `manifest.txt` exists. `--force` redoes configure and preprocess but never
re-clones. Each corpus ends with a TU-count check against the recipe's `EXPECTED_TU`,
reported as `VERIFY <corpus>: TU=n (expected m) OK|MISMATCH`.

By default it writes to `corpus-infra/sources/` and `corpus-infra/corpora/` — *not*
over the live `corpusN/` directories. Point `--outroot /tanksmall/scratch/ictmp` at
them deliberately if that is what you want.

Costs, roughly: corpus7/8/10/13/14/16 are minutes each; corpus9/11/12/15 tens of
minutes; corpus2/3/4/5 an hour or so each (large configures); corpus6 needs a ~6 min
full Godot build first; corpus needs a full LLVM configure plus the 720 s harvest
build. Requirements: git, cmake + ninja, GCC, python3, and `scons` on PATH for
corpus6 only.

## Unpacking a snapshot: `--long=31` is not optional

The archives are compressed with a 2 GiB window, and zstd refuses windows above
128 MB at *decompression* time unless you ask for them. A plain `zstd -d` fails:

```
Decoding error (36) : Frame requires too much memory for decoding
Window size larger than maximum : 2147483648 > 134217728
Use --long=31 or --memory=2048MB
```

So always:

```bash
zstd -dc --long=31 snapshots/corpusN.tar.zst | tar -x -C <dest>
```

The window is where nearly all the compression comes from (up to 10.8x over the
default 8 MB window on these corpora), so it is worth the flag — but anything that
consumes these archives has to pass it, including any tooling you point at them.

## Known non-reproducibilities

* **corpus (LLVM)** — TU count depends on how far a 720 s `-j12` build got.
* **corpus12 (Eigen)** — capped at 650 of 1542 TUs on purpose (`PP_LIMIT`); Eigen
  averages ~5.4 MB of `.ii` per TU, the largest expansion in the set, and the full
  suite is ~8 GB.
* **corpus15 (simdjson)** — 153 of 156; 3 developer-mode targets fail to preprocess
  and are dropped.
* **corpus13 (re2)** — pinned to `0dade9ff` (2021-10-28), the last commit before RE2
  took a hard Abseil dependency. Un-pinning would drag Abseil headers into this corpus
  and overlap it with corpus4.
* Unpinned projects track their default branch, so a fresh clone gives newer source
  than the commits recorded in each `METADATA.json`.
