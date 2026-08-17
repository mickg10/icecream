# Compression tournament gate matrix: current P29 control

This is the first matrix against the owner's binding whole-stream gates. Every zstd reference is one frame over ordered concatenated content, not a sum of per-TU frames. Installed-package bytes are not present in P29.

## Aggregate status

| gate | eligible | passing | reference | P29 | P29/reference | result |
|---|---:|---:|---:|---:|---:|:---:|
| cold `.ii` ≤110% z19-long per program | 16 | 3 | 97,671,668 | 91,864,787 | 0.941 | PASS aggregate / FAIL per-program |
| TU 100 ≤ z6-long per program | 10 | 5 | 14,221,568 | 14,474,741 | 1.018 | FAIL aggregate |
| TU 200 ≤ z6-long per program | 9 | 4 | 19,855,026 | 20,616,852 | 1.038 | FAIL aggregate |
| cold raw source ≤110% z19-long | 16 | 0 measured | 117,962,240 | — | — | OPEN |

Aggregate passing does not satisfy the goal: each eligible program must pass.

## Per-program matrix

| program | cold P29/z19 | cold | TU100 P29/z6 | TU100 | TU200 P29/z6 | TU200 | raw source |
|---|---:|:---:|---:|:---:|---:|:---:|:---:|
| LLVM | 1.226 | no | 1.009 | no | 1.031 | no | open |
| RocksDB | 1.550 | no | 1.155 | no | 1.150 | no | open |
| DuckDB | 1.338 | no | 0.836 | yes | 0.875 | yes | open |
| abseil+protobuf | 1.423 | no | 1.060 | no | 1.065 | no | open |
| OpenCV | 1.298 | no | 0.973 | yes | 1.078 | no | open |
| Godot | 0.679 | yes | 1.056 | no | 1.073 | no | open |
| fmt | 1.414 | no | final@50: 1.099 | n/a | final@50: 1.099 | n/a | open |
| spdlog | 1.143 | no | final@34: 0.917 | n/a | final@34: 0.917 | n/a | open |
| Catch2 | 1.187 | no | 0.870 | yes | 0.954 | yes | open |
| nlohmann-json | 1.438 | no | final@99: 1.099 | n/a | final@99: 1.099 | n/a | open |
| range-v3 | 1.141 | no | 0.895 | yes | 0.874 | yes | open |
| Eigen | 0.983 | yes | 0.902 | yes | 0.937 | yes | open |
| re2 | 1.137 | no | final@72: 0.915 | n/a | final@72: 0.915 | n/a | open |
| LevelDB | 1.237 | no | final@72: 0.970 | n/a | final@72: 0.970 | n/a | open |
| simdjson | 1.360 | no | 1.042 | no | final@153: 1.060 | n/a | open |
| cereal | 1.041 | yes | final@84: 0.828 | n/a | final@84: 0.828 | n/a | open |

## Exact byte ledger

| program | z19 `.ii` | 110% gate | P29 cold | z6 TU100 | P29 TU100 | z6 TU200 | P29 TU200 | z19 source | source gate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| LLVM | 7,608,807 | 8,369,687 | 9,328,055 | 1,593,762 | 1,607,925 | 2,884,547 | 2,975,306 | 23,389,162 | 25,728,078 |
| RocksDB | 6,337,047 | 6,970,751 | 9,823,481 | 3,840,198 | 4,435,639 | 5,414,625 | 6,229,228 | 4,159,458 | 4,575,403 |
| DuckDB | 7,167,012 | 7,883,713 | 9,589,726 | 2,129,543 | 1,781,171 | 2,847,389 | 2,491,331 | 7,269,271 | 7,996,198 |
| abseil+protobuf | 4,119,889 | 4,531,877 | 5,863,183 | 1,645,017 | 1,743,408 | 2,329,125 | 2,480,140 | 3,782,763 | 4,161,039 |
| OpenCV | 6,596,295 | 7,255,924 | 8,563,234 | 639,458 | 622,144 | 1,493,491 | 1,609,831 | 8,972,186 | 9,869,404 |
| Godot | 58,289,521 | 64,118,473 | 39,589,524 | 1,290,414 | 1,362,035 | 2,211,547 | 2,372,213 | 65,161,354 | 71,677,489 |
| fmt | 739,921 | 813,913 | 1,046,398 | — | — | — | — | 435,733 | 479,306 |
| spdlog | 472,193 | 519,412 | 539,513 | — | — | — | — | 198,813 | 218,694 |
| Catch2 | 847,768 | 932,544 | 1,006,039 | 532,201 | 463,168 | 617,728 | 589,210 | 249,816 | 274,797 |
| nlohmann-json | 812,814 | 894,095 | 1,168,818 | — | — | — | — | 406,367 | 447,003 |
| range-v3 | 766,091 | 842,700 | 874,082 | 619,941 | 554,725 | 897,361 | 783,889 | 309,032 | 339,935 |
| Eigen | 1,402,131 | 1,542,344 | 1,377,958 | 768,113 | 692,626 | 1,159,213 | 1,085,704 | 1,877,377 | 2,065,114 |
| re2 | 429,535 | 472,488 | 488,243 | — | — | — | — | 214,279 | 235,706 |
| LevelDB | 554,104 | 609,514 | 685,350 | — | — | — | — | 719,661 | 791,627 |
| simdjson | 1,036,075 | 1,139,682 | 1,408,595 | 1,162,921 | 1,211,900 | — | — | 572,413 | 629,654 |
| cereal | 492,465 | 541,711 | 512,588 | — | — | — | — | 244,555 | 269,010 |

## Provenance

- zstd: `*** Zstandard CLI (64-bit) v1.5.7, by Yann Collet ***`
- zstd prefix command: `zstd -6 --long=31 -c` with one thread
- cold reference: retained corpus catalog `zstd -19 --long=31` values
- candidate: exact first-repetition RBASE-P29 per-TU export
- all measured prefix raw-byte totals match the candidate ledger exactly
