# Rehydrating the corpus snapshots on another machine

These archives carry the icecream compression-research workload set: 16 corpora of
preprocessed C++ translation units (`.ii`) plus the 14 raw-source training roots they
came from. They exist to be moved to a bakeoff box and unpacked — they are not the
metrics measurement (that is `zstd -19 --long=31` piped straight to `wc -c`, never
stored). These are compressed with `zstd -3 --long=27 -T0` for transfer speed.

## What is in here

| file | what |
|---|---|
| `corpusN.ii.tar.zst` (x16) | one corpus of `.ii` files + its `manifest.txt` + `METADATA.json` (+ `NAME` where present), paths relative to the corpus root |
| `src-<project>.tar.zst` (x14) | one raw-source checkout, rooted at the project directory name, with `.git/`, `build/`, `CMakeFiles/` and any `*.ii` excluded |
| `SHA256SUMS` | checksums for every archive. **Written last** — its presence means every archive finished. |
| `REHYDRATE.md` | this file |

Corpora: `corpus` (LLVM), `corpus2` RocksDB, `corpus3` DuckDB, `corpus4` abseil+protobuf,
`corpus5` OpenCV, `corpus6` Godot, `corpus7` fmt, `corpus8` spdlog, `corpus9` Catch2,
`corpus10` nlohmann-json, `corpus11` range-v3, `corpus12` Eigen, `corpus13` re2,
`corpus14` LevelDB, `corpus15` simdjson, `corpus16` cereal.

Source roots: rocksdb, abseil-cpp, opencv, godot, fmt, spdlog, catch2, json, range-v3,
eigen, re2, leveldb, simdjson, cereal. (LLVM, DuckDB and protobuf ship as `.ii` only.)

## 1. Transfer

```bash
# from the machine holding the snapshots
rsync -av --partial --info=progress2 \
  /tanksmall/scratch/ictmp/corpus-snapshots/ quietbox2:/data/corpus-snapshots/
```

`--partial` matters: the archives are written atomically (`.tmp` then `mv`), so anything
matching `*.tar.zst` in the source directory is complete, but a long transfer of the
27 GB-equivalent set benefits from resumability.

## 2. Verify

```bash
cd /data/corpus-snapshots && sha256sum -c SHA256SUMS
```

If `SHA256SUMS` is absent, the producing side had not finished — do not unpack yet.

## 3. Unpack

Pick a root. Everything below assumes `DEST=/data/ictmp`.

```bash
DEST=/data/ictmp
mkdir -p "$DEST"

# .ii corpora -> $DEST/corpusN/
for f in /data/corpus-snapshots/corpus*.ii.tar.zst; do
  c=$(basename "$f" .ii.tar.zst)
  mkdir -p "$DEST/$c"
  zstd -dc "$f" | tar -x -C "$DEST/$c"
done

# source roots -> $DEST/src/<project>/
mkdir -p "$DEST/src"
for f in /data/corpus-snapshots/src-*.tar.zst; do
  zstd -dc "$f" | tar -x -C "$DEST/src"
done
```

No `--long` flag is needed to decompress these: `--long=27` is a 128 MiB window, which
is exactly zstd's default decode limit, so a plain `zstd -d` works. (This is *not* true
of anything compressed at `--long=31`; that needs `zstd -d --long=31`.)

## 4. Rewrite the manifests — REQUIRED

Each `corpusN/manifest.txt` holds **absolute** paths from the machine that built it
(`/tanksmall/scratch/ictmp/corpusN/...`). Nothing will find the files until those are
rewritten to the new root:

```bash
DEST=/data/ictmp
for m in "$DEST"/corpus*/manifest.txt; do
  sed -i 's#^/tanksmall/scratch/ictmp/#'"$DEST"'/#' "$m"
done
```

Then confirm every listed file actually exists:

```bash
for m in "$DEST"/corpus*/manifest.txt; do
  n=$(wc -l < "$m"); ok=$(xargs -a "$m" -d'\n' -n500 ls -d 2>/dev/null | wc -l)
  printf '%-40s %6d listed  %6d present %s\n' "$m" "$n" "$ok" \
    "$( [ "$n" = "$ok" ] && echo OK || echo MISMATCH )"
done
```

Expected TU counts: corpus 1238, corpus2 622, corpus3 689, corpus4 700, corpus5 1506,
corpus6 2207, corpus7 50, corpus8 34, corpus9 857, corpus10 99, corpus11 259,
corpus12 650, corpus13 72, corpus14 72, corpus15 153, corpus16 84 — 9,292 total,
27,231.9 MiB logical.

The `.ii` files themselves also contain absolute paths, in the `# <line> "<path>"`
markers the preprocessor emits. Those are **not** rewritten and should not be: they are
part of the payload being compressed, and rewriting them would change every measurement.
Only `manifest.txt` needs fixing.

## 5. Note on the bundled METADATA.json

The `METADATA.json` inside each `corpusN.ii.tar.zst` is the copy that existed when the
archive was built. The authoritative, consolidated version is
`compression-research/data/corpus-catalog.json` in the repo — use that when the numbers
matter.
