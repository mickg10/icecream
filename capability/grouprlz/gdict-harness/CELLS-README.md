# Which extracted cell tree to consume

`cells44/` — **USE THIS.** Extracted from each cell's `corpus.json`-declared payload
(`ii.tar.zst`), SHA-256 verified against that declaration *and* against local-oracle's
`capability/selector/verified-44-cells.tsv` @265ffb7 (payload sha256 + TU count + summed raw
bytes; 44/44, zero mismatches). Built by `prep44.py`.

TU counts: catch2 861, rocksdb 418, spdlog 168, re2 72, leveldb 94, nlohmann-json 99, fmt 51,
cereal 80, range-v3 537, opencv 1500, eigen 1516 (gcc) / 1522 (clang).

Also holds `abseil__*`, which is **v2-pool only** — abseil has no `corpus.json` and is not in the
authoritative 44. Ignore it unless you specifically want the bonus rows.

`cells/` — **SUPERSEDED.** Extracted from the older `<project>-<profile>.ii.tar.zst` package.
Wrong TU sets (catch2 107, rocksdb 367, spdlog 8, re2 22, leveldb 39). Retained for provenance
only. **Do not measure from this.**

Per-cell manifests are the `.man` files beside each directory: absolute paths in canonical
ordinal order, taken from the `manifest.tsv` that is the sibling of the declared payload.

Never resolve a cell by globbing `*.ii.tar.zst` — that picks the superseded package.
