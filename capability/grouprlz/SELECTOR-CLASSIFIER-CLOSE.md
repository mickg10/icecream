# Closing the classifier science

Two questions were routed to me: fold grouprlz's runway census into the
leave-one-project-out, and run the sharp test on local-oracle's 4-feature rule.

## 1. The runway census adds no discriminating power

grouprlz's census is 19 rows, of which **3 are genuinely new lineages** -- magnum (40
TUs), draco (136), taglib (148). The other 16 are the fixed-16 corpora already held.

All three new lineages are **GRZ winners with near-zero runway** (remaining_tus 0 / 24 /
36). The runway rule classifies all three correctly, so folding them in changes the
held-out total by **exactly zero**:

| family | held-out regret |
|---|---:|
| docker | 1,921,792 |
| fixed-16 | 953,356 |
| native | 1,150,996 |
| **runway (new)** | **0** |
| **TOTAL** | **4,026,144** (unchanged) |

Positive lineages are now 9 of 26 projects. **These rows confirm the easy side and test
nothing** -- they are short-runway *negatives*, and the rule was never in doubt there. The
cases that would discriminate are short-runway *positives*, and the census found none.

Meanwhile the census independently reproduces the two failures already on record:
corpus12 (eigen, 650 TUs) wins with P29 while the runway rule says GRZ, and corpus5
(opencv, 1506 TUs) wins with GRZ while the rule says P29. **No simple rule survives** --
confirmed, as expected.

## 2. The sharp test: does the 4-feature rule separate the rocksdb reversal?

**Yes, decisively.** Same codebase, two builds, opposite winners -- and the features flip
with the build, not with the project.

| build | probe raw | **trajectory** | **literal wire frac** | **root+missing frac** | winner | rule |
|---|---:|---:|---:|---:|---|---|
| rocksdb docker conan-gcc | 655,730,008 | 0.0803 | 0.6071 | 0.0486 | p29 | p29 ok |
| rocksdb docker debian-gcc | 593,521,934 | 0.0851 | 0.5810 | 0.0498 | p29 | p29 ok |
| rocksdb docker fedora-clang-libcxx | 766,552,465 | 0.0606 | 0.6544 | 0.0428 | p29 | p29 ok |
| rocksdb docker linuxbrew | 605,360,139 | 0.0638 | 0.6415 | 0.0350 | p29 | p29 ok |
| **rocksdb native fixed-16** | **739,716,080** | **0.6062** | **0.2026** | **0.3452** | **grz** | **grz ok** |

- **trajectory 0.061-0.085 vs 0.606** -- a 7-10x separation
- **literal wire fraction 0.58-0.65 vs 0.20** -- 3x
- **root+missing fraction 0.035-0.050 vs 0.345** -- 7-10x

And crucially **`probe_raw` does not separate them at all**: 594-767 MB for the P29 side
against 740 MB for the GRZ side, straddling it. So the discrimination is coming entirely
from the representation features, exactly as the design intends.

Eigen appears in both generations too and does not reverse (P29 both ways), with
trajectory 0.058-0.102 docker against 0.193 native -- same direction, consistent.

**Verdict: the 4-feature rule has real content signal.** It distinguishes two builds of one
codebase that reverse their winner, using features that are properties of *this build's
representation* rather than project identity, job count, or raw extent. That is the thing
every simple rule I tested failed to do.

### Independent corroboration of the inputs

I did not take these rows on trust. Two of local-oracle's `probe_raw_bytes` match my own
independent measurements to the byte: rocksdb native fixed-16 **739,716,080** equals my
raw@TU112 from the `.tu` boundary map, and rocksdb docker debian-gcc **593,521,934** equals
the probe raw from my own identity run. The dev rows are consistent with my measurements
wherever they overlap.

## Where this leaves the science

- **Simple rules: dead**, and now dead with more evidence rather than less. Raw extent,
  region reuse depth, census ratio, mean TU size, bytes per distinct line, literal fraction
  alone, and runway -- none separate. The reversal is the reason: the winner is a property
  of the build's content, so any feature keyed to the project or its size must fail.
- **The 4-feature rule passes the hardest test available on development data.** Its true
  test remains local-oracle's 9-project holdout, which is the right instrument and is
  already running under a no-changes-after-labels discipline.
- **None of this gates the docker result.** Policy A delivers the per-cell oracle with no
  classifier for +2% CPU, which is why the classifier question could be closed honestly
  either way.
