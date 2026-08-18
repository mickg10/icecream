# Corrected P29 native-nine holdout identity

- Exact cells: **9/9**.
- Prefix-identical cells: **9/9**.
- Corrected complete P29: **132,291,209 B**.
- TU112 probes: **13,835,928 B**.

| holdout | TUs | raw GB | probe raw MB | probe P29 MB | complete P29 MB | trajectory |
|---|---:|---:|---:|---:|---:|---:|
| arrow | 297 | 0.916 | 391.518 | 1.254 | 2.325 | 0.0541 |
| folly | 364 | 1.515 | 429.406 | 1.804 | 2.428 | 0.0555 |
| bitcoin | 621 | 2.311 | 348.399 | 1.045 | 3.507 | 0.2441 |
| gcc | 780 | 2.542 | 381.427 | 1.845 | 12.749 | 0.3176 |
| qt6 | 1205 | 7.051 | 377.080 | 1.800 | 9.310 | 0.2983 |
| pytorch | 1742 | 10.802 | 277.163 | 0.877 | 8.935 | 0.2807 |
| v8 | 3798 | 30.952 | 272.488 | 1.327 | 21.523 | 0.0139 |
| clickhouse | 11741 | 63.514 | 221.149 | 0.693 | 37.472 | 0.0530 |
| firefox | 9646 | 102.048 | 1859.880 | 3.191 | 34.043 | 0.1443 |

These projects were excluded from the selector-development inputs named by
`causal-selector-policy-v1.json`. Process timings remain diagnostic.
