# Exact BSC residual granularity across TU groups

## Result

Most of the build-wide BWT gain does **not** require the complete build. With an exact one-byte
selector over product-shaped BSC, zstd-3, and zstd-10 frames:

- DuckDB falls from `3,945,305` residual bytes at one TU/frame to
  `3,575,448` at 10 TUs, `3,419,493`
  at 100 TUs, and `3,326,695` for one complete-build group.
- Godot falls from `11,636,298` to
  `9,931,951`, `9,184,069`, and
  `8,967,267` at the same boundaries.
- A fixed 100-TU DuckDB grouping projects to `7,890,852`
  complete bytes, only `7,139` bytes above its gate.
  The first passing fixed size is `128` TUs, at
  `7,879,687`
  complete bytes.
- DuckDB's 500-TU partition is `4,657`
  bytes smaller than one 689-TU BSC block. BWT statistics benefit slightly from a boundary here;
  "one monolithic block" is therefore not automatically the compression optimum.

The engineering implication is a bounded precompute window, initially 128 TUs, with every group
charged when its first TU becomes eligible. This retains progressive reconstruction and captures
nearly all of the measured BSC compression. A byte-targeted or cost-selected boundary policy is the
next experiment because TU byte sizes vary substantially.

## Exact wire and accounting

Every nonempty group carries one four-byte outer frame, one selector byte, and the selected payload.
BSC payloads use one or more self-describing 28-byte libbsc blocks capped at 64 MiB raw. The decoder
reads only the selected payload, independently reconstructs the exact raw residual, and byte-compares
it before the row is accepted. Empty groups carry no residual frame.

The complete projections replace only the exact P29 literal-lane wire and retain every other P29
byte. They are not yet an integrated P29 lane multiplexer. Full group-by-group watermarks are retained
in the detail TSVs and validated against each summary row.

The sweep ran on `nas642` with `OMP_NUM_THREADS=1`, libbsc 3.3.12 commit
`baffa62c70b6ebbecc9af14ce550e965ea247680`. Timing columns describe that host; the size and exact-replay results are the
primary result.

## DuckDB

Exact residual: `31,439,473` raw bytes across `689` TUs. Causal P29 literal wire: `5,118,367` bytes. Fixed complete-P29 bytes outside that lane: `4,471,359`. Whole-program zstd-19-long: `7,167,012`; 110% gate: `7,883,713`.

| TUs/group | selected residual | projected complete | / whole z19 | gate delta | lane bytes over best | groups BSC/z3/z10 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 3,945,305 | 8,416,664 | 1.174x | +532,951 | 623,267 | 323/12/223 |
| 2 | 3,812,287 | 8,283,646 | 1.156x | +399,933 | 490,249 | 216/1/68 |
| 4 | 3,707,227 | 8,178,586 | 1.141x | +294,873 | 385,189 | 132/1/14 |
| 8 | 3,605,641 | 8,077,000 | 1.127x | +193,287 | 283,603 | 73/0/1 |
| 10 | 3,575,448 | 8,046,807 | 1.123x | +163,094 | 253,410 | 59/1/0 |
| 16 | 3,524,854 | 7,996,213 | 1.116x | +112,500 | 202,816 | 38/0/0 |
| 25 | 3,503,592 | 7,974,951 | 1.113x | +91,238 | 181,554 | 26/0/0 |
| 32 | 3,467,804 | 7,939,163 | 1.108x | +55,450 | 145,766 | 20/0/0 |
| 50 | 3,452,638 | 7,923,997 | 1.106x | +40,284 | 130,600 | 14/0/0 |
| 64 | 3,435,837 | 7,907,196 | 1.103x | +23,483 | 113,799 | 11/0/0 |
| 100 | 3,419,493 | 7,890,852 | 1.101x | +7,139 | 97,455 | 7/0/0 |
| 128 | 3,408,328 | 7,879,687 | 1.099x | -4,026 | 86,290 | 6/0/0 |
| 200 | 3,406,460 | 7,877,819 | 1.099x | -5,894 | 84,422 | 4/0/0 |
| 500 | 3,322,038 | 7,793,397 | 1.087x | -90,316 | 0 | 2/0/0 |
| full | 3,326,695 | 7,798,054 | 1.088x | -85,659 | 4,657 | 1/0/0 |

## Godot

Exact residual: `96,195,716` raw bytes across `2,207` TUs. Causal P29 literal wire: `13,672,703` bytes. Fixed complete-P29 bytes outside that lane: `25,916,821`. Whole-program zstd-19-long: `58,289,521`; 110% gate: `64,118,473`.

| TUs/group | selected residual | projected complete | / whole z19 | gate delta | lane bytes over best | groups BSC/z3/z10 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 11,636,298 | 37,553,119 | 0.644x | -26,565,354 | 2,669,031 | 1098/30/1005 |
| 2 | 11,066,927 | 36,983,748 | 0.634x | -27,134,725 | 2,099,660 | 830/2/255 |
| 4 | 10,520,436 | 36,437,257 | 0.625x | -27,681,216 | 1,553,169 | 514/0/31 |
| 8 | 10,064,137 | 35,980,958 | 0.617x | -28,137,515 | 1,096,870 | 272/0/2 |
| 10 | 9,931,951 | 35,848,772 | 0.615x | -28,269,701 | 964,684 | 219/0/0 |
| 16 | 9,689,361 | 35,606,182 | 0.611x | -28,512,291 | 722,094 | 137/0/0 |
| 25 | 9,498,891 | 35,415,712 | 0.608x | -28,702,761 | 531,624 | 89/0/0 |
| 32 | 9,433,855 | 35,350,676 | 0.606x | -28,767,797 | 466,588 | 69/0/0 |
| 50 | 9,314,711 | 35,231,532 | 0.604x | -28,886,941 | 347,444 | 45/0/0 |
| 64 | 9,274,995 | 35,191,816 | 0.604x | -28,926,657 | 307,728 | 35/0/0 |
| 100 | 9,184,069 | 35,100,890 | 0.602x | -29,017,583 | 216,802 | 23/0/0 |
| 128 | 9,177,298 | 35,094,119 | 0.602x | -29,024,354 | 210,031 | 18/0/0 |
| 200 | 9,109,224 | 35,026,045 | 0.601x | -29,092,428 | 141,957 | 12/0/0 |
| 500 | 9,025,991 | 34,942,812 | 0.599x | -29,175,661 | 58,724 | 5/0/0 |
| full | 8,967,267 | 34,884,088 | 0.598x | -29,234,385 | 0 | 1/0/0 |

## Scope

This experiment answers the granularity question for BSC only. The previously measured ZPAQ m5
build-wide lane projection remains much smaller, while being a different compression point. The
next complete-codec proof is to make the 128-TU residual groups first-class P29 frames, replay them
through the independent decoder, and record the compressed-input watermark at every TU boundary.
