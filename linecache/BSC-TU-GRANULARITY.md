# Exact BSC residual granularity across TU groups

## Result

Most of the build-wide BWT gain does **not** require the complete build. With an exact packed-header
selector over product-shaped BSC, zstd-3, and zstd-10 frames:

- DuckDB falls from `3,944,747` residual bytes at one TU/frame to
  `3,575,388` at 10 TUs, `3,419,486`
  at 100 TUs, and `3,326,694` for one complete-build group.
- Godot falls from `11,634,165` to
  `9,931,732`, `9,184,046`, and
  `8,967,266` at the same boundaries.
- A fixed 100-TU DuckDB grouping projects to `7,890,845`
  complete bytes, only `7,132` bytes above its gate.
  Within this coarse sweep, the first passing fixed size is `128` TUs, at
  `7,879,681`
  complete bytes.
- DuckDB's 500-TU partition is `4,658`
  bytes smaller than one 689-TU BSC block. BWT statistics benefit slightly from a boundary here;
  "one monolithic block" is therefore not automatically the compression optimum.

The engineering implication is a bounded precompute window with every group charged when its first
TU becomes eligible. A focused 104--124 sweep and complete P29 replay subsequently selected one
fixed **112-TU** boundary: it clears DuckDB cold while preserving both Godot prefix gates. See
`P29-BSC-GROUP-INTEGRATION.md` for the actual decoder-verified totals and speed repetitions.

## Exact wire and accounting

Every nonempty group carries one packed four-byte header and the selected payload. The upper three
header bits select the codec and the lower 29 bits carry the payload length.
BSC payloads use one or more self-describing 28-byte libbsc blocks capped at 64 MiB raw. The decoder
reads only the selected payload, independently reconstructs the exact raw residual, and byte-compares
it before the row is accepted. Empty groups carry no residual frame.

The complete projections in this table replace only the exact P29 literal-lane wire and retain every
other P29 byte. Full group-by-group watermarks are retained in the detail TSVs and validated against
each summary row. The later 112-TU row is integrated into P29 and reported separately.

The sweep ran on `nas642` with `OMP_NUM_THREADS=1`, libbsc 3.3.12 commit
`baffa62c70b6ebbecc9af14ce550e965ea247680`. Timing columns describe that host; the size and exact-replay results are the
primary result.

## DuckDB

Exact residual: `31,439,473` raw bytes across `689` TUs. Causal P29 literal wire: `5,118,367` bytes. Fixed complete-P29 bytes outside that lane: `4,471,359`. Whole-program zstd-19-long: `7,167,012`; 110% gate: `7,883,713`.

| TUs/group | selected residual | projected complete | / whole z19 | gate delta | lane bytes over best | groups BSC/z3/z10 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 3,944,747 | 8,416,106 | 1.174x | +532,393 | 622,711 | 323/12/223 |
| 2 | 3,812,002 | 8,283,361 | 1.156x | +399,648 | 489,966 | 216/1/68 |
| 4 | 3,707,080 | 8,178,439 | 1.141x | +294,726 | 385,044 | 132/1/14 |
| 8 | 3,605,567 | 8,076,926 | 1.127x | +193,213 | 283,531 | 73/0/1 |
| 10 | 3,575,388 | 8,046,747 | 1.123x | +163,034 | 253,352 | 59/1/0 |
| 16 | 3,524,816 | 7,996,175 | 1.116x | +112,462 | 202,780 | 38/0/0 |
| 25 | 3,503,566 | 7,974,925 | 1.113x | +91,212 | 181,530 | 26/0/0 |
| 32 | 3,467,784 | 7,939,143 | 1.108x | +55,430 | 145,748 | 20/0/0 |
| 50 | 3,452,624 | 7,923,983 | 1.106x | +40,270 | 130,588 | 14/0/0 |
| 64 | 3,435,826 | 7,907,185 | 1.103x | +23,472 | 113,790 | 11/0/0 |
| 100 | 3,419,486 | 7,890,845 | 1.101x | +7,132 | 97,450 | 7/0/0 |
| 128 | 3,408,322 | 7,879,681 | 1.099x | -4,032 | 86,286 | 6/0/0 |
| 200 | 3,406,456 | 7,877,815 | 1.099x | -5,898 | 84,420 | 4/0/0 |
| 500 | 3,322,036 | 7,793,395 | 1.087x | -90,318 | 0 | 2/0/0 |
| full | 3,326,694 | 7,798,053 | 1.088x | -85,660 | 4,658 | 1/0/0 |

## Godot

Exact residual: `96,195,716` raw bytes across `2,207` TUs. Causal P29 literal wire: `13,672,703` bytes. Fixed complete-P29 bytes outside that lane: `25,916,821`. Whole-program zstd-19-long: `58,289,521`; 110% gate: `64,118,473`.

| TUs/group | selected residual | projected complete | / whole z19 | gate delta | lane bytes over best | groups BSC/z3/z10 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 11,634,165 | 37,550,986 | 0.644x | -26,567,487 | 2,666,899 | 1098/30/1005 |
| 2 | 11,065,840 | 36,982,661 | 0.634x | -27,135,812 | 2,098,574 | 830/2/255 |
| 4 | 10,519,891 | 36,436,712 | 0.625x | -27,681,761 | 1,552,625 | 514/0/31 |
| 8 | 10,063,863 | 35,980,684 | 0.617x | -28,137,789 | 1,096,597 | 272/0/2 |
| 10 | 9,931,732 | 35,848,553 | 0.615x | -28,269,920 | 964,466 | 219/0/0 |
| 16 | 9,689,224 | 35,606,045 | 0.611x | -28,512,428 | 721,958 | 137/0/0 |
| 25 | 9,498,802 | 35,415,623 | 0.608x | -28,702,850 | 531,536 | 89/0/0 |
| 32 | 9,433,786 | 35,350,607 | 0.606x | -28,767,866 | 466,520 | 69/0/0 |
| 50 | 9,314,666 | 35,231,487 | 0.604x | -28,886,986 | 347,400 | 45/0/0 |
| 64 | 9,274,960 | 35,191,781 | 0.604x | -28,926,692 | 307,694 | 35/0/0 |
| 100 | 9,184,046 | 35,100,867 | 0.602x | -29,017,606 | 216,780 | 23/0/0 |
| 128 | 9,177,280 | 35,094,101 | 0.602x | -29,024,372 | 210,014 | 18/0/0 |
| 200 | 9,109,212 | 35,026,033 | 0.601x | -29,092,440 | 141,946 | 12/0/0 |
| 500 | 9,025,986 | 34,942,807 | 0.599x | -29,175,666 | 58,720 | 5/0/0 |
| full | 8,967,266 | 34,884,087 | 0.598x | -29,234,386 | 0 | 1/0/0 |

## Scope

This experiment answers the granularity question for BSC only. The previously measured ZPAQ m5
build-wide lane projection remains much smaller, while being a different compression point. The
complete follow-up makes 112-TU residual groups first-class P29 frames, replays them through the
independent decoder, and records the compressed-input watermark at every TU boundary.
