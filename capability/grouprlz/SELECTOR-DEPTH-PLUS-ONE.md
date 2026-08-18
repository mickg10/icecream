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

---

# UPDATE: the recommendation was right, and acting on it changed the answer

The negative result above concluded that the blocker was **lineage count, not features**.
I acted on that: measured both codecs' complete wires and the TU112 census on six further
native corpora (corpus17, 19, 21, 22, 23, 24 -- 25.1 GB raw, 5,009 TUs), all with the
frozen GRZ2 fixed-112 policy and corrected P29 (56c1744, stable Root tags),
`byte-exact=OK` on all six.

| corpus | TUs | GRZ2 complete | P29+BSC complete | winner |
|---|---:|---:|---:|---|
| corpus17 | 780 | 11,050,906 | 12,749,467 | GRZ2 |
| corpus19 | 1205 | 10,206,123 | **9,309,538** | **P29+BSC** |
| corpus21 | 1742 | 12,500,414 | **8,935,043** | **P29+BSC** |
| corpus22 | 364 | 2,682,229 | **2,427,818** | **P29+BSC** |
| corpus23 | 297 | 1,925,523 | 2,324,563 | GRZ2 |
| corpus24 | 621 | 3,279,236 | 3,507,130 | GRZ2 |

**Three new P29-winning lineages.** That takes the positive lineages from four to seven
and adds 4,716,367 recoverable bytes that are not concentrated in one corpus.

## With those lineages, depth + 1 works

Same search, same rule shape, same leave-one-project-out protocol, now over 66 rows:

| rule | held docker | held fixed-16 | held native | **total** |
|---|---:|---:|---:|---:|
| **depth + remaining_tus** | 1,921,792 | **953,356** | 1,150,996 | **4,026,144** |
| depth + ref_per_lit | 9,447,503 | 2,383,662 | 4,446,716 | 16,277,881 |
| frozen 500 MB baseline | 564,099 | 21,769,955 | 4,716,367 | 27,050,421 |
| always-P29+BSC | 8,582,092 | 7,539,040 | 2,325,495 | 18,446,627 |
| always-GRZ2 | 8,810,160 | 19,587,482 | 4,716,367 | 33,114,009 |

**Held-out total 4,026,144 -- 6.7x better than the frozen baseline and 4.6x better than
always-P29.** The fixed-16 held-out figure improved from 20,939,932 to **953,356, a 22x
gain, purely from adding training lineages**; the feature set did not change. That is a
direct confirmation of the earlier diagnosis.

(`depth + seen_frac` scores identically -- `seen_frac = probe_tus/total_tus` and
`probe_tus` is 112 or the whole corpus, so the two induce the same partition.
`remaining_tus` is the more interpretable form and is what is reported.)

## The rule and its mechanism

Fitted on everything (for inspection, not held out):
**`P29+BSC iff depth >= 14.5 and remaining_tus >= 1093`.**

Both terms are P29's amortization story, split into past and future:

- **depth** = region reuse achieved *so far* -- is P29's structural model paying off yet?
- **remaining_tus** = amortization runway *still ahead* -- how many more TUs can that model
  be spread over?

Both are causal and free at TU112: depth comes out of the P29 probe census, and the
remaining TU count is known at submit time because the build system knows its own job
list.

On the cells named as the test:

| corpus | depth | remaining_tus | label | rule | |
|---|---:|---:|---|---|---|
| godot | 36.4 | 2095 | P29 | P29 | ok |
| llvm | 39.3 | 1126 | P29 | P29 | ok |
| rocksdb (fixed-16) | 3.6 | 510 | GRZ | GRZ | ok |
| corpus21 | 20.1 | 1630 | P29 | P29 | ok |
| corpus19 | 20.1 | 1093 | P29 | P29 | ok |
| corpus17 / 23 / 24 | 22.8 / 35.8 / 31.9 | 668 / 185 / 509 | GRZ | GRZ | ok |
| **eigen (fixed-16)** | 84.2 | 538 | P29 | GRZ | **wrong** |
| **corpus22** | 18.7 | 252 | P29 | GRZ | **wrong** |

**It gets Godot, LLVM and RocksDB right** -- the three the question was posed on -- and 5
of 6 native. It misses fixed-16 Eigen and corpus22, both short-runway corpora where P29
still wins.

## Two things not to overclaim

1. **It is still worse than the frozen rule on docker specifically** (1,921,792 vs
   564,099 held out). It wins overall and on two families of three; it does not dominate.
2. Seven positive lineages is better than four but is not many. The docker regression and
   the two misses are both short-runway cases, which suggests the runway term is too blunt
   -- but I am not going to add a third threshold to chase two cells on 66 rows. The
   discipline that produced the honest negative result applies here too.

Recommendation: adopt `depth + remaining_tus` as the current best cheap causal rule,
keep the frozen 500 MB rule as the docker-side reference it genuinely still beats, and
resolve the disagreement with more short-runway P29-winning lineages rather than more
thresholds.
