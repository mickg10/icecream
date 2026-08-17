# GRZ2 fixed-112 independent acceptance replay

Date: 2026-08-17  
Role: `mickg10/local-oracle`  
Machine: `ttuser@tt-quietbox2`, pinned to logical CPUs `0-15`

## Result

The bounded-history GRZ2 codec has a fixed policy inside BigOracle's requested
`32/64/112` TU search space that passes the owner's three stated bars on both full
RocksDB and full Abseil:

```text
complete bytes <= 1.10 * whole-program zstd-19 reference
C encode       >= 1,000,000,000 bytes/s
F decode       >=   500,000,000 bytes/s, one decode worker
```

Every timed repetition passes. Both corpora replay byte-for-byte, all repeated normal
encodes produce the same container, and an independent retry/rollback encode produces
that same container. This supersedes the provisional 224-TU policy as the bounded
development candidate. The 224-TU result remains a separately labelled capability point.

## Artifact binding

The implementer artifact was commit `d743996101b97b7bc6c94b09f3dfa75a7442e0ce`.
LocalOracle then made one narrow correction commit, `b74cb04`:

1. include the alternate offset-delta stream (`sd2`) in deterministic retry comparison;
2. make the source warning-clean under `-Wall -Wextra -Wpedantic -Werror`;
3. make the executable name in build instructions and diagnostics consistently `grz2g`.

The correction does not alter a normal encoded stream. A normal encode and an encode
with `--retry-test 1` still produce identical hashes for both measured corpora.

```text
corrected source sha256
57ef6970cc15625f569f1d4ee5a2365620b1bd62aaee970c3c53b20af2286599

measured binary sha256
05c71c65d25afad331ac95d084639ea86489c3b31b440b94b95e3dc935f31621

build
g++ -O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    -I "$HOME/libbsc" -c grz2g.cpp
g++ grz2g.o "$HOME/grouprlz/libbsc.a" -lzstd -lpthread -o grz2g
```

The independent frame/state gate was run against that binary and passed in full:

```text
PASS exact replay, prefix identity, strict cuts, and frame/state closure
PASS byte-zero anchor and same-group anchor visibility
PASS rollback from provisional anchors and deterministic retry
PASS repeated physical ring wrap and composable group/whole digests
ALL GRZ2 BINDING CHECKS PASSED
```

Retained gate directory:

```text
ttuser@tt-quietbox2:/tmp/grz2-binding-local-freeze-20260817
```

## Frozen development policy

The policy is identical for both corpora; there is no corpus-name choice.

| Dimension | Value |
|---|---:|
| mode | G2 rolling committed history |
| minimum match | 256 bytes |
| anchor sampling | `s=6` |
| anchor index | `t=21`, 2,097,152 entries, 16 MiB |
| literal backend | libbsc QLFC fast (`BSC-E0`) |
| metadata backend | zstd level 12 |
| literal block | 8 MiB |
| C literal workers | 8 |
| F workers | 1 |
| maximum TUs per group | 112 |
| maximum raw bytes per group | 512 MiB |
| maximum ADD bytes per group | 128 MiB |
| retained history | 1 GiB |

Command options:

```text
-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 -j 8 \
--gtu 112 --graw 512 --gadd 128 --hist 1024
```

The 1-GiB retained history plus one active group rounds the decoder's physical ring to
2 GiB. The anchor index is 16 MiB. No measured group exceeded any configured cap and no
input contained an individually oversized TU.

## Inputs and references

The complete whole-program zstd-19 references are used; they are not the old
`INT_MAX`-truncated measurements.

| Corpus | TUs | Raw bytes | Raw SHA-256 | TU-map SHA-256 | zstd-19 bytes |
|---|---:|---:|---|---|---:|
| RocksDB | 622 | 3,114,320,596 | `3a1a403a05dbfb3533d192f733f135df0098a9952c037330f3013b21377936a8` | `5c5aaaffc9a283271d28efc16580a4978ab60f131e9886282520dda18511fde6` | 6,199,621 |
| Abseil | 700 | 2,581,008,467 | `bcb480ff7a8177daaf08d354d1d28c5915eb2a49f29fdea4682230e6ef340f44` | `f434b01991909ff0a9c9f86ad71cd2a5b64ebf1ba21d75b9cc655a036886d262` | 4,020,529 |

## Acceptance measurements

Rates below use raw bytes divided by complete wall time. `C minimum` and `F minimum`
are the slowest of three timed repetitions; the gate is therefore not based on a best run.

| Corpus | Wire bytes | / z19 | Groups | C repetitions (s) | C minimum B/s | F repetitions (s) | F minimum B/s | C/F peak RSS | Ring | Result |
|---|---:|---:|---:|---|---:|---|---:|---:|---:|:---:|
| RocksDB | 5,456,878 | 0.8802 | 7 | 2.8907, 2.9163, 2.9552 | 1,053,837,790 | 3.1574, 3.1811, 3.1875 | 977,031,917 | 1.73 / 2.07 GiB | 2.00 GiB | PASS |
| Abseil | 3,424,583 | 0.8518 | 7 | 2.1440, 2.1147, 2.1249 | 1,203,840,088 | 2.4774, 2.4645, 2.4684 | 1,041,816,821 | 1.63 / 2.05 GiB | 2.00 GiB | PASS |

Container hashes:

```text
RocksDB 895b4ed13c7cedf1d4f033d3fb0dddcfce06a0beb882caac6eeb7bfca5a54201
Abseil c25f13a49b7c1354fc6c4125c383b7ba407e815e81813293249aa0f7b3691a22
```

The complete machine-readable row is `g2-fixed112-independent.tsv`. The reusable runner
is `g2_fixed112.sh`; unlike the earlier `g012.sh`, it:

- gates the minimum rate across every repetition;
- checks the corpus ledger against physical raw and TU-map sizes;
- requires deterministic containers and identical curves across normal repetitions;
- runs exact decode and retains input/output hashes;
- runs `--retry-test 1` separately and requires its wire to equal the timed wire;
- checks the TU/raw/ADD caps from the emitted group curve;
- reports the C and F thresholds independently.

The runner itself was smoke-tested end-to-end on full Abseil:

```text
ALL FIXED-112 ROWS PASSED:
/tmp/grz2-fixed112-runner-smoke-534d7c1c/g2-fixed112.tsv
```

Full retained raw logs, all repetitions, curves, hashes, and containers:

```text
ttuser@tt-quietbox2:
/home/ttuser/grouprlz/retained/grz2g-local-freeze-20260817T2325Z
```

## TU100/TU200 transfer and lookahead

Checkpoint accounting uses two deliberately separate quantities:

- `physically_emitted_at_N` is the stream header plus frames whose source groups have
  already closed when exactly N TUs have arrived. It can cover fewer than N TUs and is
  therefore not used to claim the transfer gate.
- `charged_W` is the stream header plus every group containing any of the first N TUs.
  The whole group frame is charged to its first TU. This is the conservative transfer
  required to reconstruct the first N TUs; its future-TU and raw-byte wait are reported.

The 36-byte final END frame is not charged at an intermediate checkpoint. Every
`charged_W` cut ends exactly after a complete GROUP frame and contains no END frame.

Fresh references were generated with zstd CLI 1.4.8 using `-6 --long=31 -T0` over the
exact concatenation of the first N TUs. No old prefix reference was reused.

| Corpus | N | Raw at N | Physically emitted | Physical material through | Charged W | Material available through | Lookahead | Raw lookahead | zstd-6 long | W/z6 | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| RocksDB | 100 | 669,183,165 | 1,762,650 | TU82 | 2,794,136 | TU166 | 66 TU | 396,769,368 | 3,644,530 | 0.7667 | PASS |
| RocksDB | 200 | 1,224,557,334 | 2,794,136 | TU166 | 3,263,577 | TU271 | 71 TU | 374,311,726 | 5,099,578 | 0.6400 | PASS |
| Abseil | 100 | 284,336,916 | 72 | TU0 | 1,049,411 | TU112 | 12 TU | 33,736,287 | 1,572,322 | 0.6674 | PASS |
| Abseil | 200 | 548,396,320 | 1,049,411 | TU112 | 1,414,663 | TU224 | 24 TU | 74,831,446 | 2,220,670 | 0.6370 | PASS |

`verify_g2_checkpoints.py` independently validates the curve against the binary TU map
and physical container, creates each cut, runs `decprefix`, and compares the decoded
bytes with the exact raw prefix through the reported material boundary. All four cuts
replayed exactly. The same END-less cuts were also passed to strict `dec`; all four were
correctly rejected with `full decode requires END_FRAME`.

Machine-readable artifacts:

- `g2-fixed112-checkpoints.tsv`: the four checkpoint rows, hashes, and exactness results;
- `g2-fixed112-group-charges.tsv`: every group's first-TU charge, completion boundary,
  ADD size, raw size, retained-history extent, and close reason;
- `verify_g2_checkpoints.py`: the independent cut/replay verifier.

Retained zstd frames, zstd timing/hashes, exact cut containers, decode logs, and generated
ledgers are under:

```text
ttuser@tt-quietbox2:
/home/ttuser/grouprlz/retained/grz2g-local-freeze-20260817T2325Z/checkpoints-v1
```

The standalone prefix-decoder wall rates are not endpoint-rate measurements. Each test
starts a fresh process and allocates a new 2-GiB ring to decode only one or two groups;
the live design retains the F store across group arrivals. Full-build one-worker F rates
remain the endpoint gate recorded above.

## What this proves and what remains

This closes the two-corpus bounded-policy existence question at the requested maximum
112-TU group cap. It also closes exact frame replay, suffix-independent prefix framing,
rollback, ring wrap, digest closure, deterministic output, cold size, endpoint rates, and
TU100/TU200 transfer accounting for these two development corpora.

It does not yet finish Issue #16. The remaining acceptance work is:

1. run the frozen policy over the fixed 16, then the available broader corpus set;
2. compare bounded GRZ2 with P29+BSC under a chronological selector, while keeping the
   complete-program minimum labelled only as a ceiling;
3. add the eventual live-input adapter and a maximum group wait before product use. The
   current capability codec maps the input file and models group chronology, but it is not
   itself the compiler-pipe implementation.
