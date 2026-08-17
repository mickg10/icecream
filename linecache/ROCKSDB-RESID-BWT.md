# RocksDB residual-BWT closure result

## Result

Fast block sorting substantially improves RocksDB's exact P29 literal residual, but it does not
close the cold whole-program gate. RocksDB therefore joins Abseil and fmt in the remaining
structure/prediction set.

| candidate | residual wire | projected complete | / whole `.ii` z19 | delta to 110% gate | exact |
|---|---:|---:|---:|---:|:---:|
| P29 causal zstd-3 stream | 3,521,608 | 9,823,481 | 1.550x | +2,852,730 | yes |
| zstd-19-long31 residual | 2,464,628 | 8,766,501 | 1.383x | +1,795,750 | yes |
| libbsc b64/m0/e0 | 2,150,824 | 8,452,697 | 1.334x | +1,481,946 | yes |
| libbsc b64/m0/e1 | 2,107,830 | 8,409,703 | 1.327x | +1,438,952 | yes |
| **libbsc b64/m0/e2** | **2,090,346** | **8,392,219** | **1.324x** | **+1,421,468** | **yes** |

The references are 6,337,047 bytes for one `zstd -19 --long=31` frame over all RocksDB `.ii`
content and 6,970,751 bytes for its 110% gate. The projection replaces only P29's measured
3,521,608-byte residual lane and keeps the remaining 6,301,873 charged bytes unchanged.

The 30,118,741-byte residual is 0.9671% of RocksDB's 3,114,320,596 raw `.ii` bytes. Libbsc e2 is
374,282 bytes (15.19%) smaller than residual z19-long and 1,431,262 bytes (40.64%) smaller than the
live P29 lane. The remaining 1.42 MB miss is outside the reach of another modest entropy-backend
gain: the repeated material has already been split into definitions and references in a way that
loses some whole-program cross-TU match opportunities.

## Exact run

The residual was emitted by the current independent P29 encoder/decoder using the canonical 622-TU
manifest and `--residual-dump`. Its SHA-256 is
`a364b6a4a3e820785bf4065a14ed1043c9e8e0185ea653170ec4953da0b6774b`.

All four candidate archives were independently decoded and byte-compared. Libbsc 3.3.12 e2 encoded
at 67.04 MB/s and decoded at 123.01 MB/s on quietbox2; e0/e1 encoded at 74.50/71.36 MB/s. Runtime is
not the limiting dimension for this row—the byte gate is.

Machine-readable accounting and retained paths are in
[`rocksdb-resid-bwt.json`](ml-artifacts/rocksdb-resid-bwt.json).

## Decision

Keep fast BWT as a composable residual block because it closes several other corpora and improves
RocksDB strongly. Do not tune another RocksDB-only residual backend expecting it to recover 1.42 MB.
The remaining candidate should preserve or predict cross-TU structure before the literal material is
fragmented, always compared against the existing complete-byte fallback.
