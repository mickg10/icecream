# Heterogeneous `.ii` corpus matrix

This harness builds the same C/C++ projects under deliberately different preprocessing
environments.  Its purpose is to answer one narrow question for issue #16:

> Can a pre-shared, exact cross-Line/superblock model trained in two environments improve
> unseen projects in two unseen environments?

The matrix is separate from the protocol implementation.  It produces immutable corpus
archives and metadata that any codec implementation can consume.

## Non-negotiable experiment boundaries

1. A durable corpus is a single `*.ii.tar.zst` archive plus its SHA-256.  Loose `.ii`
   files are temporary and are removed after archive replay verifies every TU.
2. Each archive contains one complete project/profile corpus and is compressed as one
   stream with `zstd -6 --long=31`.  Archive size is a distribution/storage measurement,
   not protocol wire accounting.
3. TU order is explicit.  Files are renamed `ii/00000000.ii`, `ii/00000001.ii`, ... in
   build/compile-database order, and `manifest.tsv` records each exact byte count and digest.
4. A preprocessing profile is identified by the compiler, compiler version, target,
   sysroot, standard library, include search path, predefined macros, dependency source,
   container image ID, and source/build mount prefixes.  An image with two compilers is
   two profiles.
5. Checkout prefixes intentionally differ between profiles.  Path-bearing records must be
   parameterized and reconstructed exactly; a model does not get credit for memorizing one
   host prefix.
6. A generic test target is excluded from model training in every training environment.
   Same-project cross-environment results are reported separately and never labelled generic.
7. Installed package bytes are outside per-build transfer accounting.  Runtime memory,
   encode/decode time, exact output, and all build-path bytes are still measured.

## Four initial profiles

`profiles.tsv` defines the initial systems:

| profile | compiler/dependency character | source prefix |
|---|---|---|
| `debian-gcc` | Debian packages, GCC, libstdc++ | `/src/debian/project` |
| `fedora-clang-libcxx` | Fedora packages, Clang, libc++ | `/opt/fedora/worktree` |
| `linuxbrew` | Linuxbrew compiler, tools, and libraries | `/home/linuxbrew/worktree` |
| `conan-gcc` | Conan 2 locked source-built dependencies, GCC | `/workspace/conan/source` |

These are intentionally not four cosmetic base images.  They change the compiler built-ins,
system headers, standard library, configured feature macros, dependency headers, and checkout
paths.  A future native macOS or other non-Linux runner is a fifth profile; a Linux container
does not emulate another kernel/SDK.

Large projects keep their native build systems.  Conan and Linuxbrew describe where external
dependencies/tooling come from; they are not requirements that Firefox, GCC, V8, or every other
project replace its own bootstrap mechanism.

## Cell layout and lifetime

During a build, one host directory is bind-mounted at `/cell`:

```text
<matrix-root>/<project>/<profile>/
  build/                 temporary build and generated files
  loose/                 temporary .ii output
  raw-manifest.txt       temporary ordered absolute paths
  commands.jsonl         retained compile/preprocess commands
  logs/                  retained build and preprocessing logs
  environment.json       retained profile/project fingerprint
  environment/           compiler macro/include-path captures
  stage/                 temporary canonical archive tree
  <project>-<profile>.ii.tar.zst
  <project>-<profile>.ii.tar.zst.sha256
```

After `package_corpus.sh` creates the archive, it decodes the whole archive into a temporary
directory and verifies every `manifest.tsv` digest.  Only then may `build/`, `loose/`, and
`stage/` be removed.  The archive, digest, commands, logs, and environment fingerprint remain.

On `quietbox2`, use local NVMe:

```sh
export II_MATRIX_ROOT=/home/ttuser/ictmp/ii-matrix
```

Compressed archives can then be copied to durable storage under `tanksmall`.  There is no
reason to keep the expanded matrix resident.

## Adapter contract

Every project adapter is invoked inside a profile container with:

```text
SOURCE_ROOT   profile-specific bind path containing the checkout
BUILD_ROOT    /cell/build
LOOSE_ROOT    /cell/loose
CELL_ROOT     /cell
JOBS          requested parallelism
CC / CXX      profile compiler
```

It must:

1. run the project's native configure/code-generation steps;
2. preprocess in deterministic compile-database/build order;
3. write each successful `.ii` under `LOOSE_ROOT`;
4. write ordered cell-relative `.ii` paths to `/cell/raw-manifest.txt`;
5. write the effective commands to `/cell/commands.jsonl`;
6. return nonzero if the accepted TU-count contract is not met.

The existing 25 corpus recipes are the starting adapters, but expected counts are profile-specific:
generated files and conditionally enabled TUs legitimately differ.  A cell is comparable only when
its exact project revision and accepted target set are recorded.

## Archive format

`package_corpus.sh` canonicalizes and packages:

```text
manifest.tsv
environment.json
commands.jsonl
environment/*
logs/*
ii/00000000.ii
ii/00000001.ii
...
```

`manifest.tsv` columns are:

```text
ordinal  relative_path  raw_bytes  sha256  original_path
```

The archive command is fixed:

```sh
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    --format=posix --pax-option=delete=atime,delete=ctime -C stage -cf - . \
  | zstd -6 --long=31 -T0 -o <archive>.tmp
```

The `.tmp` is renamed only after complete decode and per-TU verification.  Readers use
`unpack_corpus.sh`, which verifies the outer archive digest and all inner TU digests before
writing an absolute `manifest.txt` for existing codec tools.

## Training and evaluation splits

For four profiles A/B/C/D, run three complementary environment splits:

```text
AB -> CD
AC -> BD
AD -> BC
```

Projects are assigned to deterministic folds.  For generic evaluation of target fold `k`, no
project in fold `k` appears in the training profiles.  This creates two report families:

```text
same-project cross-environment
    train P@A,P@B; test P@C,P@D

generic project+environment holdout
    train projects not in fold(P) at A,B; test P@C,P@D
```

The second family is the universal-package result.  `make_splits.py` emits machine-readable TSV
rows and refuses overlapping train/test cells.

## Model ladder

The archive harness does not assume one model.  At minimum evaluate:

```text
E0  empty online state
R0  raw-source-trained zstd dictionary on the residual literal lane
I0  .ii-trained zstd dictionaries on homogeneous streams
I1  .ii-trained immutable whole-TU raw-content prefix (cross-Line byte-history control)
S0  pre-shared parameterized cross-Line superblocks
S1  S0 plus causal project-local online superblocks
M0  encoder-only candidate/rule ranker over S1
```

`R0`/`I0` are short-context controls.  `I1` is built from complete ordered training TUs rather
than isolated Line samples and is rebound as immutable history for each target TU.  It measures
literal cross-Line recurrence without claiming parameterized structure; training/test project and
profile exclusions remain unchanged.  The principal candidate is `S0`/`S1`, whose installed
package contains decoder-executable rules:

```text
SUPERBLOCK_RULE {
    ordered multi-Line or multi-Region expansion;
    nested rule references with bounded depth;
    equality links for repeated slots;
    typed path, identifier, number, string, and raw-byte slots;
}
```

A static rule is eligible only when it has support in multiple projects and in both training
profiles.  Counts from one project/path/profile cannot promote it.  C emits a rule ID plus exact
slot and residual streams; F expands the installed rule and verifies the expected output extent.
After TU `t` is completely encoded/decoded, both peers may promote matching project-local rules for
TU `t+1`.  Current-TU information may define message-local rules but never persistent future state
until the TU is committed.

## Required report

For every target corpus/profile/model row retain:

- byte-exact reconstruction result;
- per-TU and cumulative build-path wire bytes;
- checkpoints at TU 50, 100, 150, 200, 250, 300, and full build;
- cumulative raw/wire ratio and bytes saved versus empty start;
- ideal payload time at 1 Gbit/s (`bytes / 125000000`) and 1 GB/s (`bytes / 1000000000`);
- encode/decode throughput and peak memory;
- bytes by root/control/rule/slot/literal/array/blob/missing/frame category;
- rule support by project and profile;
- standard order, fixed reorder, one-line dependency-root change, revert, and repeated-build curves.

The installed package has zero build-wire bytes and its installed size is not part of the requested
comparison.  It still has a version/digest so both endpoints select the same rule namespace.

## Commands

```sh
# Build/fingerprint the four images.
compression-research/ii-matrix/scripts/build_images.sh

# On a host where Docker is reached through sudo:
DOCKER='sudo docker' compression-research/ii-matrix/scripts/build_images.sh

# Run one adapter-defined cell.
compression-research/ii-matrix/scripts/run_cell.sh \
  debian-gcc firefox /path/to/gecko adapter-script.sh

# Package and immediately replay-verify one completed cell.
compression-research/ii-matrix/scripts/package_corpus.sh \
  /home/ttuser/ictmp/ii-matrix/firefox/debian-gcc firefox debian-gcc

# Expand later for a reader and print the absolute codec manifest.
compression-research/ii-matrix/scripts/unpack_corpus.sh \
  firefox-debian-gcc.ii.tar.zst /tmp/firefox-debian-gcc

# Emit the environment/project holdout plan.
python3 compression-research/ii-matrix/scripts/make_splits.py \
  --profiles compression-research/ii-matrix/profiles.tsv \
  --projects compression-research/ii-matrix/projects.tsv \
  --out /tmp/ii-matrix-splits.tsv
```

## Current status

The harness and contracts are implemented here.  The original 25 corpora already resident on
`quietbox2` remain useful controls, but they are not substitutes for the four-profile matrix.  The
100 project/profile adapter runs and cross-profile learned-superblock results are follow-on compute;
they must not be reported as complete until every referenced archive exists and replays exactly.
