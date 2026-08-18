# Corrected P29 fixed-16 prefix identity

- Exact cells: **16/16**.
- Prefix-identical cells: **16/16**.
- Corrected complete P29: **78,541,028 B**.
- Corrected TU112/complete-program probes: **17,195,369 B**.

| corpus | TUs | probe raw MB | probe P29 MB | complete P29 MB | trajectory | mode |
|---|---:|---:|---:|---:|---:|:---:|
| llvm | 1238 | 404.434 | 1.381 | 7.276 | 0.0772 | suffix-blind |
| rocksdb | 622 | 739.716 | 4.161 | 8.534 | 0.6062 | suffix-blind |
| duckdb | 689 | 363.350 | 1.530 | 7.877 | 0.0507 | suffix-blind |
| abseil | 700 | 318.073 | 1.711 | 5.056 | 0.3277 | suffix-blind |
| opencv | 1506 | 121.562 | 0.663 | 6.737 | 0.3182 | suffix-blind |
| godot | 2207 | 219.325 | 1.074 | 35.085 | 0.2649 | suffix-blind |
| fmt | 50 | 136.350 | 0.923 | 0.923 | 0.0699 | complete-program |
| spdlog | 34 | 98.384 | 0.478 | 0.478 | 0.0487 | complete-program |
| catch2 | 857 | 118.480 | 0.434 | 0.907 | 0.1364 | suffix-blind |
| nlohmann-json | 99 | 293.918 | 1.012 | 1.012 | 0.0944 | complete-program |
| range-v3 | 259 | 276.082 | 0.478 | 0.739 | 0.0596 | suffix-blind |
| eigen | 650 | 612.714 | 0.737 | 1.144 | 0.1925 | suffix-blind |
| re2 | 72 | 110.231 | 0.436 | 0.436 | 0.0490 | complete-program |
| leveldb | 72 | 143.868 | 0.616 | 0.616 | 0.1577 | complete-program |
| simdjson | 153 | 337.574 | 1.101 | 1.260 | 0.0404 | suffix-blind |
| cereal | 84 | 326.899 | 0.459 | 0.459 | 0.0048 | complete-program |

These are corrected deterministic size/feature rows. Process timings remain diagnostic
and are not the isolated common-input selector measurement.
