# Cold retained-state P25-P29 transfer matrix

This supersedes the earlier 240-row presentation. The matrix contains exactly **80 cold rows: 16 projects × P25 through P29**. Bit-0 and bit-1 are separate half-cache acceptance evidence and are intentionally not repeated here.

Each codec runs one manifest containing the same project build four times in the same order. F starts empty for build 1 and retains learned objects for builds 2--4. `full build` is the cumulative endpoint after build 1; `4× full build` is the cumulative endpoint after all four builds, not merely the fourth-build increment.

Every TU reconstructs exactly. Every first-build endpoint is required to equal its previously retained complete P25, P26, P27, P28, or P29 cold ledger byte-for-byte. Thus repeating the manifest did not change any first-build decision.

Cells show cumulative decimal MB and computed ideal payload time at 1 Gbit/s. The link rate is fixed at 125,000,000 bytes/s; these times are calculations, not codec-throughput measurements.

## Schema legend

| schema | complete-codec change |
|---|---|
| `p25-cold` | key-map material baseline |
| `p26-cold` | compressed zlib-member recovery |
| `p27-cold` | P26 plus canonical-MO factoring |
| `p28-cold` | P27 plus S1 chain 1024 |
| `p29-cold` | P28 plus selected blob zstd-9/LDM workers |

## Matrix

| project | schema | TU 50 | TU 100 | TU 150 | TU 200 | TU 250 | TU 300 | full build | 4× full build |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| llvm | `p25-cold` | 1.3 MB · 0.010 s | 1.7 MB · 0.013 s | 2.4 MB · 0.019 s | 3.1 MB · 0.024 s | 3.3 MB · 0.026 s | 3.5 MB · 0.028 s | 9.6 MB · 0.077 s | 9.7 MB · 0.078 s |
| llvm | `p26-cold` | 1.2 MB · 0.010 s | 1.6 MB · 0.013 s | 2.4 MB · 0.019 s | 3.0 MB · 0.024 s | 3.2 MB · 0.026 s | 3.4 MB · 0.028 s | 9.3 MB · 0.075 s | 9.4 MB · 0.075 s |
| llvm | `p27-cold` | 1.2 MB · 0.010 s | 1.6 MB · 0.013 s | 2.4 MB · 0.019 s | 3.0 MB · 0.024 s | 3.2 MB · 0.026 s | 3.4 MB · 0.028 s | 9.3 MB · 0.075 s | 9.4 MB · 0.075 s |
| llvm | `p28-cold` | 1.2 MB · 0.010 s | 1.6 MB · 0.013 s | 2.4 MB · 0.019 s | 3.0 MB · 0.024 s | 3.2 MB · 0.026 s | 3.4 MB · 0.028 s | 9.3 MB · 0.075 s | 9.4 MB · 0.075 s |
| llvm | `p29-cold` | 1.2 MB · 0.010 s | 1.6 MB · 0.013 s | 2.4 MB · 0.019 s | 3.0 MB · 0.024 s | 3.2 MB · 0.026 s | 3.4 MB · 0.028 s | 9.3 MB · 0.075 s | 9.4 MB · 0.075 s |
| rocksdb | `p25-cold` | 2.3 MB · 0.019 s | 5.5 MB · 0.044 s | 6.9 MB · 0.055 s | 7.7 MB · 0.062 s | 8.4 MB · 0.067 s | 8.7 MB · 0.070 s | 12 MB · 0.096 s | 12 MB · 0.096 s |
| rocksdb | `p26-cold` | 2.0 MB · 0.016 s | 4.4 MB · 0.035 s | 5.6 MB · 0.045 s | 6.2 MB · 0.050 s | 6.8 MB · 0.054 s | 7.1 MB · 0.057 s | 9.8 MB · 0.079 s | 9.9 MB · 0.079 s |
| rocksdb | `p27-cold` | 2.0 MB · 0.016 s | 4.4 MB · 0.035 s | 5.6 MB · 0.045 s | 6.2 MB · 0.050 s | 6.8 MB · 0.054 s | 7.1 MB · 0.057 s | 9.8 MB · 0.079 s | 9.9 MB · 0.079 s |
| rocksdb | `p28-cold` | 2.0 MB · 0.016 s | 4.4 MB · 0.035 s | 5.6 MB · 0.045 s | 6.2 MB · 0.050 s | 6.8 MB · 0.054 s | 7.1 MB · 0.057 s | 9.8 MB · 0.079 s | 9.9 MB · 0.079 s |
| rocksdb | `p29-cold` | 2.0 MB · 0.016 s | 4.4 MB · 0.035 s | 5.6 MB · 0.045 s | 6.2 MB · 0.050 s | 6.8 MB · 0.054 s | 7.1 MB · 0.057 s | 9.8 MB · 0.079 s | 9.9 MB · 0.079 s |
| duckdb | `p25-cold` | 1.7 MB · 0.013 s | 1.8 MB · 0.015 s | 1.9 MB · 0.015 s | 2.5 MB · 0.020 s | 2.6 MB · 0.021 s | 3.3 MB · 0.027 s | 10 MB · 0.082 s | 10 MB · 0.083 s |
| duckdb | `p26-cold` | 1.6 MB · 0.013 s | 1.8 MB · 0.014 s | 1.9 MB · 0.015 s | 2.5 MB · 0.020 s | 2.6 MB · 0.021 s | 3.3 MB · 0.026 s | 9.6 MB · 0.077 s | 9.6 MB · 0.077 s |
| duckdb | `p27-cold` | 1.6 MB · 0.013 s | 1.8 MB · 0.014 s | 1.9 MB · 0.015 s | 2.5 MB · 0.020 s | 2.6 MB · 0.021 s | 3.3 MB · 0.026 s | 9.6 MB · 0.077 s | 9.6 MB · 0.077 s |
| duckdb | `p28-cold` | 1.6 MB · 0.013 s | 1.8 MB · 0.014 s | 1.9 MB · 0.015 s | 2.5 MB · 0.020 s | 2.6 MB · 0.021 s | 3.3 MB · 0.026 s | 9.6 MB · 0.077 s | 9.6 MB · 0.077 s |
| duckdb | `p29-cold` | 1.6 MB · 0.013 s | 1.8 MB · 0.014 s | 1.9 MB · 0.015 s | 2.5 MB · 0.020 s | 2.6 MB · 0.021 s | 3.3 MB · 0.026 s | 9.6 MB · 0.077 s | 9.6 MB · 0.077 s |
| abseil | `p25-cold` | 1.0 MB · 0.008 s | 2.0 MB · 0.016 s | 2.6 MB · 0.020 s | 2.9 MB · 0.024 s | 3.4 MB · 0.027 s | 4.0 MB · 0.032 s | 6.8 MB · 0.054 s | 6.9 MB · 0.055 s |
| abseil | `p26-cold` | 0.9 MB · 0.007 s | 1.7 MB · 0.014 s | 2.2 MB · 0.017 s | 2.5 MB · 0.020 s | 2.8 MB · 0.023 s | 3.3 MB · 0.026 s | 5.9 MB · 0.047 s | 5.9 MB · 0.047 s |
| abseil | `p27-cold` | 0.9 MB · 0.007 s | 1.7 MB · 0.014 s | 2.2 MB · 0.017 s | 2.5 MB · 0.020 s | 2.8 MB · 0.023 s | 3.3 MB · 0.026 s | 5.9 MB · 0.047 s | 5.9 MB · 0.047 s |
| abseil | `p28-cold` | 0.9 MB · 0.007 s | 1.7 MB · 0.014 s | 2.2 MB · 0.017 s | 2.5 MB · 0.020 s | 2.8 MB · 0.023 s | 3.3 MB · 0.026 s | 5.9 MB · 0.047 s | 5.9 MB · 0.047 s |
| abseil | `p29-cold` | 0.9 MB · 0.007 s | 1.7 MB · 0.014 s | 2.2 MB · 0.017 s | 2.5 MB · 0.020 s | 2.8 MB · 0.023 s | 3.3 MB · 0.026 s | 5.9 MB · 0.047 s | 5.9 MB · 0.047 s |
| opencv | `p25-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 1.3 MB · 0.010 s | 1.7 MB · 0.014 s | 2.3 MB · 0.018 s | 2.6 MB · 0.021 s | 8.9 MB · 0.072 s | 9.1 MB · 0.073 s |
| opencv | `p26-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 1.2 MB · 0.009 s | 1.6 MB · 0.013 s | 2.2 MB · 0.018 s | 2.5 MB · 0.020 s | 8.6 MB · 0.069 s | 8.7 MB · 0.070 s |
| opencv | `p27-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 1.2 MB · 0.009 s | 1.6 MB · 0.013 s | 2.2 MB · 0.018 s | 2.5 MB · 0.020 s | 8.6 MB · 0.069 s | 8.7 MB · 0.070 s |
| opencv | `p28-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 1.2 MB · 0.009 s | 1.6 MB · 0.013 s | 2.2 MB · 0.018 s | 2.5 MB · 0.020 s | 8.6 MB · 0.069 s | 8.7 MB · 0.070 s |
| opencv | `p29-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 1.2 MB · 0.009 s | 1.6 MB · 0.013 s | 2.2 MB · 0.018 s | 2.5 MB · 0.020 s | 8.6 MB · 0.069 s | 8.7 MB · 0.070 s |
| godot | `p25-cold` | 1.1 MB · 0.009 s | 1.4 MB · 0.011 s | 1.6 MB · 0.013 s | 2.4 MB · 0.019 s | 3.5 MB · 0.028 s | 6.0 MB · 0.048 s | 57 MB · 0.45 s | 57 MB · 0.45 s |
| godot | `p26-cold` | 1.1 MB · 0.008 s | 1.4 MB · 0.011 s | 1.6 MB · 0.012 s | 2.4 MB · 0.019 s | 3.5 MB · 0.028 s | 5.8 MB · 0.047 s | 45 MB · 0.36 s | 45 MB · 0.36 s |
| godot | `p27-cold` | 1.1 MB · 0.008 s | 1.4 MB · 0.011 s | 1.6 MB · 0.012 s | 2.4 MB · 0.019 s | 3.5 MB · 0.028 s | 5.8 MB · 0.047 s | 42 MB · 0.33 s | 42 MB · 0.34 s |
| godot | `p28-cold` | 1.1 MB · 0.008 s | 1.4 MB · 0.011 s | 1.6 MB · 0.012 s | 2.4 MB · 0.019 s | 3.5 MB · 0.028 s | 5.8 MB · 0.047 s | 42 MB · 0.33 s | 42 MB · 0.34 s |
| godot | `p29-cold` | 1.1 MB · 0.008 s | 1.4 MB · 0.011 s | 1.6 MB · 0.012 s | 2.4 MB · 0.019 s | 3.5 MB · 0.028 s | 5.6 MB · 0.045 s | 40 MB · 0.32 s | 40 MB · 0.32 s |
| fmt | `p25-cold` | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | — | — | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s |
| fmt | `p26-cold` | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | — | — | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s |
| fmt | `p27-cold` | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | — | — | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s |
| fmt | `p28-cold` | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | — | — | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s |
| fmt | `p29-cold` | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s | — | — | 1.0 MB · 0.008 s | 1.0 MB · 0.008 s |
| spdlog | `p25-cold` | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | — | — | — | — | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s |
| spdlog | `p26-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | — | — | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| spdlog | `p27-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | — | — | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| spdlog | `p28-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | — | — | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| spdlog | `p29-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | — | — | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| catch2 | `p25-cold` | 0.4 MB · 0.003 s | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.7 MB · 0.005 s | 0.7 MB · 0.005 s | 1.1 MB · 0.009 s | 1.2 MB · 0.009 s |
| catch2 | `p26-cold` | 0.4 MB · 0.003 s | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 1.0 MB · 0.008 s | 1.1 MB · 0.008 s |
| catch2 | `p27-cold` | 0.4 MB · 0.003 s | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 1.0 MB · 0.008 s | 1.1 MB · 0.008 s |
| catch2 | `p28-cold` | 0.4 MB · 0.003 s | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 1.0 MB · 0.008 s | 1.1 MB · 0.008 s |
| catch2 | `p29-cold` | 0.4 MB · 0.003 s | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 0.6 MB · 0.005 s | 1.0 MB · 0.008 s | 1.1 MB · 0.008 s |
| nlohmann-json | `p25-cold` | 1.0 MB · 0.008 s | 1.2 MB · 0.010 s | 1.2 MB · 0.010 s | 1.2 MB · 0.010 s | 1.2 MB · 0.010 s | 1.2 MB · 0.010 s | 1.2 MB · 0.010 s | 1.2 MB · 0.010 s |
| nlohmann-json | `p26-cold` | 1.0 MB · 0.008 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s |
| nlohmann-json | `p27-cold` | 1.0 MB · 0.008 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s |
| nlohmann-json | `p28-cold` | 1.0 MB · 0.008 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s |
| nlohmann-json | `p29-cold` | 1.0 MB · 0.008 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s | 1.2 MB · 0.009 s |
| range-v3 | `p25-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.005 s | 0.7 MB · 0.006 s | 0.8 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.008 s |
| range-v3 | `p26-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.004 s | 0.7 MB · 0.006 s | 0.8 MB · 0.006 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s |
| range-v3 | `p27-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.004 s | 0.7 MB · 0.006 s | 0.8 MB · 0.006 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s |
| range-v3 | `p28-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.004 s | 0.7 MB · 0.006 s | 0.8 MB · 0.006 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s |
| range-v3 | `p29-cold` | 0.5 MB · 0.004 s | 0.6 MB · 0.004 s | 0.7 MB · 0.006 s | 0.8 MB · 0.006 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s | 0.9 MB · 0.007 s |
| eigen | `p25-cold` | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | 1.0 MB · 0.008 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.2 MB · 0.010 s | 1.4 MB · 0.011 s | 1.5 MB · 0.012 s |
| eigen | `p26-cold` | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.9 MB · 0.007 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| eigen | `p27-cold` | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.9 MB · 0.007 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| eigen | `p28-cold` | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.9 MB · 0.007 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| eigen | `p29-cold` | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.9 MB · 0.007 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.1 MB · 0.009 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| re2 | `p25-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| re2 | `p26-cold` | 0.4 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| re2 | `p27-cold` | 0.4 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| re2 | `p28-cold` | 0.4 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| re2 | `p29-cold` | 0.4 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | — | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| leveldb | `p25-cold` | 0.5 MB · 0.004 s | 0.8 MB · 0.006 s | 0.8 MB · 0.006 s | 0.8 MB · 0.006 s | 0.8 MB · 0.006 s | — | 0.8 MB · 0.006 s | 0.8 MB · 0.006 s |
| leveldb | `p26-cold` | 0.5 MB · 0.004 s | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | — | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s |
| leveldb | `p27-cold` | 0.5 MB · 0.004 s | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | — | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s |
| leveldb | `p28-cold` | 0.5 MB · 0.004 s | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | — | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s |
| leveldb | `p29-cold` | 0.5 MB · 0.004 s | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | 0.7 MB · 0.006 s | — | 0.7 MB · 0.005 s | 0.7 MB · 0.006 s |
| simdjson | `p25-cold` | 1.1 MB · 0.009 s | 1.2 MB · 0.010 s | 1.4 MB · 0.012 s | 1.5 MB · 0.012 s | 1.5 MB · 0.012 s | 1.5 MB · 0.012 s | 1.5 MB · 0.012 s | 1.5 MB · 0.012 s |
| simdjson | `p26-cold` | 1.1 MB · 0.009 s | 1.2 MB · 0.010 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| simdjson | `p27-cold` | 1.1 MB · 0.009 s | 1.2 MB · 0.010 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| simdjson | `p28-cold` | 1.1 MB · 0.009 s | 1.2 MB · 0.010 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| simdjson | `p29-cold` | 1.1 MB · 0.009 s | 1.2 MB · 0.010 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s | 1.4 MB · 0.011 s |
| cereal | `p25-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| cereal | `p26-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| cereal | `p27-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| cereal | `p28-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |
| cereal | `p29-cold` | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s | 0.5 MB · 0.004 s |

## Aggregate retained-state learning

The build columns below are incremental wire bytes for each pass. The final column is the ratio of four complete raw builds to their cumulative wire.

| schema | build 1 | build 2 | build 3 | build 4 | cumulative 4× | build-1 ratio | cumulative ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| `p25-cold` | 114 MB · 0.91 s | 0.4 MB · 0.004 s | 0.1 MB · 0.001 s | 0.1 MB · 0.001 s | 115 MB · 0.92 s | 250.84× | 996.84× |
| `p26-cold` | 97 MB · 0.78 s | 0.5 MB · 0.004 s | 0.1 MB · 0.001 s | 0.1 MB · 0.001 s | 98 MB · 0.78 s | 294.33× | 1168.33× |
| `p27-cold` | 94 MB · 0.75 s | 0.5 MB · 0.004 s | 0.1 MB · 0.001 s | 0.1 MB · 0.001 s | 95 MB · 0.76 s | 303.84× | 1205.76× |
| `p28-cold` | 94 MB · 0.75 s | 0.5 MB · 0.004 s | 0.1 MB · 0.001 s | 0.1 MB · 0.001 s | 95 MB · 0.76 s | 303.87× | 1205.91× |
| `p29-cold` | 92 MB · 0.73 s | 0.5 MB · 0.004 s | 0.1 MB · 0.001 s | 0.1 MB · 0.001 s | 93 MB · 0.74 s | 310.83× | 1233.30× |

## Execution and accounting

```text
decimal_MB = cumulative_complete_protocol_wire_bytes / 1,000,000
ideal_seconds_at_1_Gbit/s = cumulative_complete_protocol_wire_bytes / 125,000,000
```

The machine TSV retains exact byte counts, computed seconds, per-build incremental wire, cumulative endpoints, ratios, and log/curve hashes.

The source-visible execution profile uses 70 quietbox2 rows and 10 capture-host rows. OpenCV and LevelDB retain original absolute source paths, so all five cold schemas for each were rerun on the capture host. This keeps material decisions consistent across stages.

Codec source SHA-256: `fa9dc5851cf24088ee00dc75efda7faf8c8c480f839fe46a6c341fffa2e510b4`. Runner source SHA-256: `7f534fdaf358ba54f4b82072cb4a2bfc6f59a9a222e0e71693f0b6d7b632faea`. Quietbox2 binary SHA-256: `d3fd6737691abc9911e550bd6a4d3f50bb599eb13a57d2e92241dfc05e3c78f5`. Capture-host binary SHA-256: `954c55d69a5ab7a954b4969ba8fbae8602708cda6fae650a94afd9653220a007`.

The 80 curves and 80 complete logs are retained under `/tanksmall/scratch/ictmp/cold-four-build-matrix-p25-p29-source-visible`.
