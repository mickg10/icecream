# Current-GRZ fixed-16 selector probes

- Exact cells: **16/16**.
- Frozen boundary: first **min(112, complete TUs)**.
- Probe raw bytes: **4,620,961,001 B**.
- Probe wire bytes: **12,982,407 B**.

| corpus | probe TUs | raw MB | GRZ MB | ADD/raw | anchor match | groups | close |
|---|---:|---:|---:|---:|---:|---:|:---|
| llvm | 112 | 404.434 | 1.217 | 0.0260 | 0.4178 | 1 | tu |
| rocksdb | 112 | 739.716 | 2.241 | 0.0824 | 0.0442 | 2 | raw,tu |
| duckdb | 112 | 363.350 | 1.427 | 0.0334 | 0.2976 | 1 | tu |
| abseil | 112 | 318.073 | 1.049 | 0.0763 | 0.2305 | 1 | tu |
| opencv | 112 | 121.562 | 0.620 | 0.0548 | 0.4456 | 1 | tu |
| godot | 112 | 219.325 | 0.951 | 0.0388 | 0.4824 | 1 | tu |
| fmt | 50 | 136.350 | 0.662 | 0.0697 | 0.3576 | 1 | tu |
| spdlog | 34 | 98.384 | 0.436 | 0.0442 | 0.4263 | 1 | tu |
| catch2 | 112 | 118.480 | 0.389 | 0.0312 | 0.7171 | 1 | tu |
| nlohmann-json | 99 | 293.918 | 0.743 | 0.0340 | 0.5764 | 1 | tu |
| range-v3 | 112 | 276.082 | 0.410 | 0.0171 | 0.7273 | 1 | tu |
| eigen | 112 | 612.714 | 0.658 | 0.0108 | 0.2960 | 2 | raw,tu |
| re2 | 72 | 110.231 | 0.401 | 0.0328 | 0.7247 | 1 | tu |
| leveldb | 72 | 143.868 | 0.490 | 0.0413 | 0.5697 | 1 | tu |
| simdjson | 112 | 337.574 | 0.865 | 0.0272 | 0.4511 | 1 | tu |
| cereal | 84 | 326.899 | 0.424 | 0.0129 | 0.4632 | 1 | tu |

The endpoint rates above are deliberately retained only in the TSV as diagnostics;
the selector features contain no measured wall-clock input.
