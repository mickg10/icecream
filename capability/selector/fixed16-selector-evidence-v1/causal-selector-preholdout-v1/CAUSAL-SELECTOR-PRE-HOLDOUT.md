# Pre-holdout causal selector candidate

The thresholds in this report are frozen before replaying native corpora 17-25.
No project name, profile, timing, complete-size, or future-TU field is an input.

```text
P29 when
  (probe_raw >= 500,000,000 and trajectory <= 0.30)
or
  (probe_raw >= 200,000,000
   and literal/wire >= 0.57
   and (Root + missing-request)/wire <= 0.05
   and trajectory <= 0.30)
else GRZ
```

| data | selected B | hindsight B | regret B | selected/z19 | raw/selected | correct |
|---|---:|---:|---:|---:|---:|---:|
| matrix44 | 63,436,306 | 62,587,653 | 848,653 | 0.983662x | 1175.025x | 39/44 |
| fixed16 | 71,015,154 | 71,015,154 | 0 | 0.740721x | 402.093x | 16/16 |
| combined | 134,451,460 | 133,602,807 | 848,653 | 0.838420x | 766.774x | 55/60 |

## Development mistakes

| data | project | generation | choice | winner | regret B |
|---|---|---|:---:|:---:|---:|
| matrix44 | cereal | conan-gcc | p29 | grz | 96,359 |
| matrix44 | cereal | debian-gcc | p29 | grz | 92,071 |
| matrix44 | cereal | linuxbrew | p29 | grz | 94,384 |
| matrix44 | opencv | fedora-clang-libcxx | grz | p29 | 465,216 |
| matrix44 | range-v3 | fedora-clang-libcxx | grz | p29 | 100,623 |

The fixed-16 result reaches the separate 400x score exactly because it chooses
P29 for LLVM, Godot, and Eigen and current GRZ for every other native row.
The nine named native holdouts are the next result; these thresholds must not be
retuned after their labels are observed.
