# Which extracted cell tree to consume

`cells44/` — **the only extracted tree, and the authoritative one.** Extracted from each cell's
`corpus.json`-declared payload (`ii.tar.zst`), SHA-256 verified against that declaration *and*
against local-oracle's `capability/selector/verified-44-cells.tsv` @265ffb7 (payload sha256 + TU
count + summed raw bytes; 44/44, zero mismatches). Built by `prep44.py`.

TU counts: catch2 861, rocksdb 418, spdlog 168, re2 72, leveldb 94, nlohmann-json 99, fmt 51,
cereal 80, range-v3 537, opencv 1500, eigen 1516 (gcc) / 1522 (clang).

Also holds `abseil__*`, which is **v2-pool only** — abseil has no `corpus.json` and is not in the
authoritative 44. Ignore it unless you specifically want the bonus rows.

Per-cell manifests are the `.man` files beside each directory: absolute paths in canonical
ordinal order, taken from the `manifest.tsv` that is the sibling of the declared payload.

## The trap this tree exists to avoid

Every matrix cell directory holds **two** tarballs, and the older one sorts first under a
`*.ii.tar.zst` glob:

| file | vintage | example |
|---|---|---|
| `ii.tar.zst` | **CURRENT** — declared by `corpus.json` | rocksdb 418 TU, catch2 861 |
| `<project>-<profile>.ii.tar.zst` | older, superseded | rocksdb 367 TU, catch2 107 |

The superseded package carries **its own internal manifest**, so "read the manifest inside the
tar" yields the wrong TU set with no error anywhere.

**Rule: resolve the payload from `corpus.json` → `payload.path`, verify SHA-256 against
`payload.sha256`, and use the `manifest.tsv` that is the sibling of the declared payload. Never
glob `*.ii.tar.zst`.**

A `cells/` tree extracted from the superseded package used to sit beside this one; it was deleted
2026-08-18 rather than left as a footgun. `prep44.py` rebuilds any tree from the declared
payloads if it is ever needed.
