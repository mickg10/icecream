# depth + 1: a negative result, and why the metric cannot decide it

Asked: find one second causal TU112 feature that, with region reuse depth, resolves the
docker overlap and beats the frozen 500 MB baseline on both corpora.

**Answer: no such feature was found, and the evidence base cannot currently validate one.**

## The search

Rule shape fixed at `P29+BSC iff depth >= D and f2 (>=|<=) F2` -- two thresholds, no model.
Nine candidate second features, all causal and cheap at TU112: `remaining_tus`,
`seen_frac`, absolute `regions`, `regions_per_mb`, `distinct_per_region`, GRZ
`addfrac`, P29 `litfrac`, the census `ratio`, and `ref_per_lit`. Validation holds whole
**projects** out -- a project's four docker profiles and its fixed-16 generation always
move together.

Baseline regret (bytes above the per-cell oracle; lower is better):

| family | frozen 500 MB | always-GRZ | always-P29 |
|---|---:|---:|---:|
| docker-44 | **564,099** | 8,810,160 | 8,582,092 |
| fixed-16 | 21,769,955 | 19,587,482 | **7,539,040** |

Held-out (leave-one-project-out) regret, best five of nine:

| rule | held docker | held fixed-16 |
|---|---:|---:|
| depth + seen_frac | 1,921,792 | 20,939,932 |
| depth + ref_per_lit | 2,779,404 | 20,884,930 |
| depth + remaining_tus | 3,040,977 | 21,221,487 |
| depth + regions | 8,001,249 | 20,941,849 |
| depth + ratio | 10,618,913 | 20,043,495 |

**Not one beats the frozen baseline on docker** -- the best is 3.4x worse (1,921,792 vs
564,099). On fixed-16 every one is far worse than the trivial always-P29 (7,539,040).

Fitted on everything (not held out, shown only for inspection), the best rule is
`depth >= 14.5 and seen_frac <= 0.0905`. It scores docker regret 823,797 -- still **worse
than frozen** -- and fixed-16 953,356, and it gets **Eigen wrong**, one of the three
fixed-16 P29 winners. A rule that is worse than the baseline on one corpus and misses a
named target on the other is not a candidate.

## Why: the metric is one project per family

Total recoverable bytes (regret of always-GRZ, i.e. everything a classifier could win):

| family | total | top contributor |
|---|---:|---|
| fixed-16 | 19,587,482 | **godot 18,333,398 = 93.6%** (eigen 4.6%, llvm 1.8%) |
| docker-44 | 8,810,160 | **eigen x4 = 86.5%** (opencv/fedora 5.3%) |

**Over 86% of the recoverable bytes in each family come from a single project.** So
byte-regret on fixed-16 is very nearly a one-corpus test of "did you get Godot right",
and on docker it is "did you get Eigen right". That is exactly why leave-one-project-out
collapses: holding out Godot leaves fixed-16 with almost no signal to fit, and holding
out Eigen does the same to docker.

Any two-threshold rule scored this way is being fitted to one project per family and
then tested on the other. The apparent successes -- my earlier fixed-16 band curiosity,
and the frozen rule's own docker score -- are both artefacts of that concentration.

## What this does and does not overturn

Unchanged: **region reuse depth is still the right causal variable.** It separates
RocksDB (3.6) from LLVM/Godot/Eigen (36.4/39.3/84.2) on fixed-16, and it tracks RocksDB's
label reversal across generations (3.6 -> 32.8-56.0). That mechanism stands on its own
evidence and does not depend on any threshold.

What fails is the step from mechanism to a *decision rule validated by byte-regret*. The
blocker is not the feature set -- it is that there are only four P29-winning lineages
(eigen, rocksdb, opencv, range-v3) and the byte metric is dominated by one of them per
family.

## Recommendation

More features will not fix this; more **P29-winning lineages** will. Concretely:
native-25 under this contract, and any additional corpora whose shape resembles Godot
(large, low cross-TU redundancy). Until the metric has several independent large P29
wins per family, I would not promote any classifier over the frozen 500 MB baseline, and
I would keep the baseline exactly where local-oracle put it: a zero-cost reference, not a
selector.

Also worth surfacing for the goal itself: on fixed-16 the trivial **always-P29+BSC**
policy has regret 7,539,040 -- three times better than the frozen rule and better than
always-GRZ. On docker the ordering reverses. The two families want opposite constant
policies, which is a cleaner statement of the same difficulty.
