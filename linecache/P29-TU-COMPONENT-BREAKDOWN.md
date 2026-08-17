# P29 exact per-TU transfer-component breakdown

`codec50 --component-curve-tsv` records every charged byte in 20 disjoint wire components after each TU. Each row is decoder-verified, and each row's components must sum exactly to its canonical complete-transfer byte count. The complete ledger is in [`p29-component-curve.tsv`](ml-artifacts/p29-component-curve.tsv).

The tables below are the causal cold run: TU *n* can use only state established before or during TU *n*. They are not a precomputed-build projection.

## Full-run composition

| program | TUs | raw `.ii` | complete wire | literal | blob | array | region control | structure | request/association | remaining |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| DuckDB | 689 | 1,985,715,205 | 9,589,726 | 5,118,367 | 0 | 1,319,885 | 2,016,440 | 668,334 | 461,144 | 5,556 |
| Godot | 2,207 | 5,932,762,185 | 39,589,524 | 13,672,703 | 14,617,593 | 6,246,413 | 4,164,025 | 616,954 | 251,973 | 19,863 |

## DuckDB: cumulative bytes through each turn

| through TU | complete wire | literal | blob | array | region control | structure | request/association | source | framing | other |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 87,856 | 10,094 | 0 | 0 | 71,325 | 4,115 | 2,313 | 0 | 8 | 1 |
| 10 | 830,100 | 461,315 | 0 | 158 | 331,237 | 25,488 | 11,817 | 0 | 76 | 9 |
| 25 | 1,012,019 | 596,341 | 0 | 228 | 368,290 | 33,109 | 13,846 | 0 | 184 | 21 |
| 50 | 1,636,194 | 685,982 | 0 | 492,399 | 402,070 | 39,764 | 15,549 | 0 | 384 | 46 |
| 100 | 1,781,171 | 779,154 | 0 | 492,399 | 440,568 | 49,843 | 18,477 | 0 | 664 | 66 |
| 150 | 1,862,265 | 821,185 | 0 | 492,399 | 468,577 | 58,620 | 20,429 | 0 | 964 | 91 |
| 200 | 2,491,331 | 1,241,669 | 0 | 567,898 | 576,213 | 77,778 | 26,313 | 0 | 1,328 | 132 |
| 250 | 2,590,440 | 1,307,998 | 0 | 567,994 | 600,374 | 83,931 | 28,393 | 0 | 1,600 | 150 |
| 300 | 3,288,839 | 1,385,508 | 0 | 1,140,301 | 633,566 | 95,492 | 31,897 | 0 | 1,900 | 175 |
| 400 | 4,709,859 | 2,285,919 | 0 | 1,316,868 | 894,029 | 157,160 | 52,913 | 0 | 2,696 | 274 |
| 500 | 7,515,459 | 4,141,012 | 0 | 1,317,280 | 1,524,162 | 349,170 | 179,965 | 0 | 3,496 | 374 |
| 689 | 9,589,726 | 5,118,367 | 0 | 1,319,885 | 2,016,440 | 668,334 | 461,144 | 0 | 4,996 | 560 |

### DuckDB: bytes added by turn interval

| TU interval | complete wire | literal | blob | array | region control | structure | request/association | source | framing | other |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1–50 | 1,636,194 | 685,982 | 0 | 492,399 | 402,070 | 39,764 | 15,549 | 0 | 384 | 46 |
| 51–100 | 144,977 | 93,172 | 0 | 0 | 38,498 | 10,079 | 2,928 | 0 | 280 | 20 |
| 101–150 | 81,094 | 42,031 | 0 | 0 | 28,009 | 8,777 | 1,952 | 0 | 300 | 25 |
| 151–200 | 629,066 | 420,484 | 0 | 75,499 | 107,636 | 19,158 | 5,884 | 0 | 364 | 41 |
| 201–250 | 99,109 | 66,329 | 0 | 96 | 24,161 | 6,153 | 2,080 | 0 | 272 | 18 |
| 251–300 | 698,399 | 77,510 | 0 | 572,307 | 33,192 | 11,561 | 3,504 | 0 | 300 | 25 |
| 301–400 | 1,421,020 | 900,411 | 0 | 176,567 | 260,463 | 61,668 | 21,016 | 0 | 796 | 99 |
| 401–500 | 2,805,600 | 1,855,093 | 0 | 412 | 630,133 | 192,010 | 127,052 | 0 | 800 | 100 |
| 501–689 | 2,074,267 | 977,355 | 0 | 2,605 | 492,278 | 319,164 | 281,179 | 0 | 1,500 | 186 |

Per-TU complete-wire distribution: p50 3,102 B; p90 28,896 B; p99 242,055 B; maximum 577,062 B.

Largest turns:

| TU | raw `.ii` | complete wire | dominant family | dominant bytes |
|---:|---:|---:|---|---:|
| 2 | 5,434,195 | 577,062 | literal | 347,817 |
| 635 | 15,786,220 | 548,603 | structure | 165,515 |
| 35 | 3,697,406 | 450,579 | array | 447,430 |
| 283 | 10,085,962 | 351,146 | array | 338,209 |
| 500 | 10,967,926 | 329,618 | region control | 99,946 |
| 290 | 4,151,779 | 242,551 | array | 234,098 |
| 469 | 7,161,752 | 242,055 | literal | 206,707 |
| 356 | 7,893,133 | 223,848 | literal | 173,749 |
| 189 | 1,368,463 | 178,685 | literal | 169,145 |
| 499 | 7,594,342 | 171,187 | literal | 58,289 |

## Godot: cumulative bytes through each turn

| through TU | complete wire | literal | blob | array | region control | structure | request/association | source | framing | other |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 340,345 | 206,319 | 0 | 0 | 122,544 | 7,279 | 4,194 | 0 | 8 | 1 |
| 10 | 529,084 | 355,280 | 0 | 0 | 155,937 | 11,832 | 5,945 | 0 | 80 | 10 |
| 25 | 863,849 | 582,974 | 19,746 | 422 | 235,268 | 17,197 | 8,017 | 0 | 200 | 25 |
| 50 | 1,059,470 | 739,616 | 19,746 | 422 | 267,861 | 21,795 | 9,580 | 0 | 400 | 50 |
| 100 | 1,362,035 | 937,708 | 19,746 | 422 | 358,870 | 31,343 | 13,046 | 0 | 800 | 100 |
| 150 | 1,554,220 | 1,066,356 | 19,746 | 463 | 410,618 | 39,910 | 15,777 | 0 | 1,200 | 150 |
| 200 | 2,372,213 | 1,648,523 | 19,746 | 104,095 | 527,071 | 51,691 | 19,287 | 0 | 1,600 | 200 |
| 250 | 3,491,481 | 2,543,317 | 19,746 | 104,158 | 717,972 | 75,096 | 28,942 | 0 | 2,000 | 250 |
| 300 | 5,638,176 | 3,224,902 | 1,273,170 | 125,956 | 885,503 | 92,404 | 33,541 | 0 | 2,400 | 300 |
| 400 | 6,471,293 | 3,886,533 | 1,273,170 | 125,956 | 1,022,767 | 118,052 | 41,215 | 0 | 3,200 | 400 |
| 500 | 7,109,817 | 4,377,181 | 1,273,170 | 125,956 | 1,139,461 | 141,100 | 48,449 | 0 | 4,000 | 500 |
| 2,207 | 39,589,524 | 13,672,703 | 14,617,593 | 6,246,413 | 4,164,025 | 616,954 | 251,973 | 0 | 17,656 | 2,207 |

### Godot: bytes added by turn interval

| TU interval | complete wire | literal | blob | array | region control | structure | request/association | source | framing | other |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1–50 | 1,059,470 | 739,616 | 19,746 | 422 | 267,861 | 21,795 | 9,580 | 0 | 400 | 50 |
| 51–100 | 302,565 | 198,092 | 0 | 0 | 91,009 | 9,548 | 3,466 | 0 | 400 | 50 |
| 101–150 | 192,185 | 128,648 | 0 | 41 | 51,748 | 8,567 | 2,731 | 0 | 400 | 50 |
| 151–200 | 817,993 | 582,167 | 0 | 103,632 | 116,453 | 11,781 | 3,510 | 0 | 400 | 50 |
| 201–250 | 1,119,268 | 894,794 | 0 | 63 | 190,901 | 23,405 | 9,655 | 0 | 400 | 50 |
| 251–300 | 2,146,695 | 681,585 | 1,253,424 | 21,798 | 167,531 | 17,308 | 4,599 | 0 | 400 | 50 |
| 301–400 | 833,117 | 661,631 | 0 | 0 | 137,264 | 25,648 | 7,674 | 0 | 800 | 100 |
| 401–500 | 638,524 | 490,648 | 0 | 0 | 116,694 | 23,048 | 7,234 | 0 | 800 | 100 |
| 501–2,207 | 32,479,707 | 9,295,522 | 13,344,423 | 6,120,457 | 3,024,564 | 475,854 | 203,524 | 0 | 13,656 | 1,707 |

Per-TU complete-wire distribution: p50 3,024 B; p90 18,873 B; p99 112,071 B; maximum 10,482,245 B.

Largest turns:

| TU | raw `.ii` | complete wire | dominant family | dominant bytes |
|---:|---:|---:|---|---:|
| 526 | 102,633,493 | 10,482,245 | blob | 10,178,658 |
| 889 | 24,534,966 | 2,952,776 | array | 2,887,901 |
| 519 | 15,072,061 | 2,753,066 | array | 2,747,743 |
| 532 | 25,229,237 | 2,373,060 | blob | 2,296,709 |
| 258 | 12,057,749 | 1,327,711 | blob | 1,253,424 |
| 536 | 7,985,337 | 723,799 | blob | 698,540 |
| 1,535 | 4,010,724 | 395,314 | literal | 357,363 |
| 1 | 2,364,113 | 340,345 | literal | 206,319 |
| 1,669 | 3,103,089 | 252,927 | literal | 201,015 |
| 215 | 4,039,995 | 249,201 | literal | 218,637 |

## Fully precomputed material-lane ceiling

This experiment concatenates each raw material lane in manifest order and compresses it as one build-wide archive. It keeps every non-material byte from the exact causal P29 ledger unchanged. Every archive was independently decoded and SHA-256 checked. The complete numbers are projections until this layout is wired into the independent decoder.

The projection is conservative about the unchanged structure and request bytes, but optimistic about knowing the complete build before transfer. It does not make any retained receiver state or model bytes free.

### DuckDB precomputed lanes

| lane | raw bytes | current causal wire | z19-long | ZPAQ m3 | ZPAQ m4 | ZPAQ m5 |
|---|---:|---:|---:|---:|---:|---:|
| region control | 4,810,857 | 2,016,440 | 1,693,733 | 1,637,334 | 1,383,485 | 1,250,919 |
| literal | 31,439,473 | 5,118,367 | 3,547,566 | 3,423,243 | 2,657,612 | 2,372,254 |
| array control | 220,290 | 12,001 | 8,898 | 7,888 | 7,888 | 7,143 |
| array values | 3,701,352 | 1,307,884 | 1,109,514 | 1,141,994 | 944,988 | 879,914 |

Unchanged charged wire outside these lanes: 1,135,034 bytes.

| complete projection | bytes | / whole-program z19 | delta to 110% gate |
|---|---:|---:|---:|
| current causal P29 | 9,589,726 | 1.338x | +1,706,013 |
| one z19-long frame/lane | 7,494,745 | 1.046x | -388,968 |
| one ZPAQ m3 archive/lane | 7,345,493 | 1.025x | -538,220 |
| best z19/m3 per lane | 7,313,013 | 1.020x | -570,700 |
| one ZPAQ m4 archive/lane | 6,129,007 | 0.855x | -1,754,706 |
| one ZPAQ m5 archive/lane | 5,645,264 | 0.788x | -2,238,449 |

### Godot precomputed lanes

| lane | raw bytes | current causal wire | z19-long | ZPAQ m3 | ZPAQ m4 | ZPAQ m5 |
|---|---:|---:|---:|---:|---:|---:|
| region control | 10,142,549 | 4,164,025 | 3,449,533 | 3,235,274 | 2,790,415 | 2,497,763 |
| literal | 96,195,716 | 13,672,703 | 9,272,575 | 9,241,472 | 6,909,348 | 6,169,401 |
| array control | 2,981,838 | 538,609 | 372,203 | 327,576 | 327,576 | 307,733 |
| array values | 7,780,568 | 5,707,804 | 5,387,926 | 5,233,878 | 5,059,346 | 4,844,578 |
| inflated embedded blobs | 124,407,478 | 14,617,593 | 12,961,679 | 14,330,468 | 11,630,246 | 10,513,795 |

Unchanged charged wire outside these lanes: 888,790 bytes.

| complete projection | bytes | / whole-program z19 | delta to 110% gate |
|---|---:|---:|---:|
| current causal P29 | 39,589,524 | 0.679x | -24,528,949 |
| one z19-long frame/lane | 32,332,706 | 0.555x | -31,785,767 |
| one ZPAQ m3 archive/lane | 33,257,458 | 0.571x | -30,861,015 |
| best z19/m3 per lane | 31,888,669 | 0.547x | -32,229,804 |
| one ZPAQ m4 archive/lane | 27,605,721 | 0.474x | -36,512,752 |
| one ZPAQ m5 archive/lane | 25,222,060 | 0.433x | -38,896,413 |

## Interpretation

The curve is lumpy rather than a smooth learning curve. A few turns introduce large literal, numeric-array, or compressed-blob populations; later turns then reuse them. This is why an average bytes/TU number hides the actual opportunity. A precomputed build sequence can batch each material lane into a build-wide stream and retain cross-TU matches. It must still send enough framing/control information for the receiver to reconstruct each TU, and all such bytes must be charged.

A build-wide archive has no honest causal marginal-byte assignment: an early match can change bytes emitted much later. Therefore this report keeps the real causal per-turn ledger intact. A precomputed-sequence result should be reported separately as either (a) an up-front charged archive, or (b) independently decodable charged chunks with their exact TU ranges.

## Reproduction

```text
codec50 <P29 flags> --component-curve-tsv CURVE.tsv
python3 linecache/summarize_component_curves.py \
  --curve DuckDB=linecache/ml-artifacts/p29-component-curve.tsv \
  --curve Godot=linecache/ml-artifacts/p29-component-curve.tsv \
  --detail-tsv linecache/ml-artifacts/p29-component-curve.tsv \
  --precomputed linecache/ml-artifacts/precomputed-material-lane-ceiling.json \
  --json linecache/ml-artifacts/p29-component-curve-summary.json \
  --markdown linecache/P29-TU-COMPONENT-BREAKDOWN.md
```
