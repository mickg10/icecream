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

---

# CORRECTION: the depth term is inert. The working rule is one counter, not two.

I reported `depth + remaining_tus` as a depth+1 result. An ablation the reviewer's
question prompted shows that is wrong, and I am withdrawing the framing.

## Depth does not separate on any family, and inverts on one

| family | P29-win depth range | GRZ-win depth range | verdict |
|---|---|---|---|
| fixed-16 | 36.4 - 84.2 | 3.6 - 72.5 | overlapping |
| docker-44 | 16.2 - 89.6 | 5.1 - 74.1 | overlapping |
| **native-6** | **18.7 - 20.1** | **22.8 - 35.8** | **INVERTED -- P29 winners have LOWER depth** |

On the native family the P29 winners (corpus19 20.1, corpus21 20.1, corpus22 18.7) all
sit *below* every GRZ winner (corpus17 22.8, corpus24 31.9, corpus23 35.8). The proposed
law -- high region reuse implies P29 amortizes and wins -- is contradicted, cleanly, by
the third family.

## The ablation: depth contributes nothing

Leave-one-project-out over all 66 rows:

| rule | held docker | held fixed-16 | held native | **total** |
|---|---:|---:|---:|---:|
| depth + remaining_tus | 1,921,792 | 953,356 | 1,150,996 | **4,026,144** |
| **remaining_tus ALONE** | 1,921,792 | 953,356 | 1,150,996 | **4,026,144** |
| depth ALONE | 3,530,971 | 20,360,843 | 6,414,928 | 30,306,742 |

**Identical to the byte.** The `depth >= 14.5` term never binds on any held-out decision;
every row that the runway threshold admits already clears it. My "depth + 1" rule was
`remaining_tus` with an inert companion, and depth alone is worse than the frozen
baseline (30,306,742 vs 27,050,421).

## What actually survives

A **single causal counter**: `P29+BSC iff remaining_tus >= ~1093`, i.e. P29 is worth
choosing only when enough TUs remain after the decision point for its structural model to
amortize. Held out it is 4,026,144 -- **6.7x better than the frozen 500 MB rule and 4.6x
better than always-P29** -- and it still gets Godot, LLVM and RocksDB right. It is one
threshold on a number the build system already knows at submit time.

What does **not** survive is the mechanism story. Region reuse depth remains a real
measurement -- RocksDB is genuinely 3.6 on fixed-16 and 32.8-56.0 on docker, and that
reversal is real -- but it does not predict the label across families and it earns no
place in a rule. I over-read a two-family pattern as a causal law; the third family
refutes it.

## Caveats the reviewer asked to record

- **The frozen rule's 0.9793x docker score is an eigen-dominance artifact.** Eigen's four
  docker profiles are 86.5% of everything recoverable on that family, so scoring well
  there mostly means classifying Eigen correctly. The same applies to the earlier
  fixed-16 band curiosity (Godot alone is 93.6% of fixed-16). Neither is a validated
  selector; both are reports about one project.
- **The three families want opposite constant policies.** always-P29+BSC is the best
  constant on fixed-16 by 3x (7,539,040 vs frozen 21,769,955) and on native
  (2,325,495 vs 4,716,367); the frozen rule is far the best on docker (564,099 vs
  8,582,092). A metric where the best constant policy flips between families is not yet
  able to certify a universal rule, however good the held-out number looks.

---

# FOLD AUDIT: the corpus19 fold FLIPS, and most positive lineages fail held out

Asked: `remaining_tus = 1093` is exactly corpus19's value, so is the threshold fit to the
project it judges? **Yes. The corpus19 fold flips.**

```
threshold refit WITHOUT corpus19 = 1411   (all-data threshold was 1093)
corpus19 remaining_tus = 1093, label P29, rule says GRZ  ->  FLIPS, regret 896,585
```

23 of 24 folds fit 1093; only the fold that holds corpus19 out fits anything else. So the
threshold is stable *because corpus19 pins it at exactly its own value* -- textbook
single-point fitting.

**The reported held-out total is not contaminated by this.** The leave-one-project-out
protocol already charged corpus19's fold its full 896,585 regret; 4,026,144 includes the
failure. The headline number stands. What does not stand is any impression that the rule
reliably identifies P29 winners.

## Held out, the rule gets 3 of 7 positive lineages right

| held-out lineage | fitted thr | outcome | regret |
|---|---:|---|---:|
| godot | 1093 | correct | 0 |
| llvm | 1093 | correct | 0 |
| corpus21 | 1093 | correct | 0 |
| **corpus19** | **1411** | **flips to GRZ** | 896,585 |
| **corpus22** | 1093 | wrong (runway 252) | 254,411 |
| **eigen** | 1093 | docker x4 correct, fixed-16 wrong (runway 538) | 898,354 |
| **rocksdb** | 1093 | all 4 docker wrong (P29 -> GRZ) | 628,189 |
| **opencv** | 1093 | 3 of 4 docker wrong (GRZ -> P29, false positives) | 1,248,629 |
| **range-v3** | 1093 | 1 of 4 wrong | 99,976 |
| 15 negative lineages | 1093 | all correct | 0 |

Only **godot, llvm and corpus21** survive being held out. The largest single fold regret
is opencv at 1,248,629 -- and those are *false positives*, the rule shipping P29+BSC where
GRZ2 was smaller, which the earlier framing never surfaced.

## Verdict

`remaining_tus >= T` beats the frozen baseline by 6.7x mainly because **frozen is
catastrophic on fixed-16 and native**, not because the rule is itself reliable. It is a
better reference point than frozen, and it is not a selector. I would state it as:

> the only cheap causal signal that survives contact with three families is "P29 needs
> runway", it is worth roughly 4 MB of the 33 MB available across 66 rows, and its
> threshold is currently pinned by a single corpus.

This strengthens, rather than weakens, the standing recommendation: **do not promote any
classifier over the frozen baseline**, and get more short-runway P29-winning lineages
before fitting anything further. Combined with the earlier finding that the depth term is
inert and inverts on native, the honest position is that **no validated selector rule
exists yet on this evidence base.**
