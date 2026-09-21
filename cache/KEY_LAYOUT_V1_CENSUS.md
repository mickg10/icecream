# KeyLayoutV1 quick runway census

This is a deliberately bounded sanity check, not a new compression experiment. It reads the
checked-in 19-corpus runway table and corrected Firefox job trace. The conservative comparison
charges one new object ordinal for every raw input byte; real Line, Region, Block, and other
objects are much coarser and repeated content reuses an earlier key.

| checked-in input | builds/TUs | raw bytes |
|---|---:|---:|
| `research/measurements/runway-census.tsv` | 19 corpora / 9,616 TUs | 29,012,306,127 |
| `research/measurements/firefox-corrected.compile-trace.tsv` | 1 build / 2,498 TUs | 15,240,876,398 |
| combined conservative charge | 12,114 TUs | 44,253,182,525 |

KeyLayoutV1 is derived in `protocol50.h`: 5 type bits, 10 generation bits, and 49 ordinal
bits. One `(type, generation)` therefore has **562,949,953,421,311** usable nonzero ordinals.
Even the one-object-per-byte charge consumes **0.007861%** of one generation, leaving a
**12,721.1x** runway. There are 1,024 generation values, including generation zero; old
and new generations may coexist. This check supports the 5/10/49 split without importing any
product Key64 assumptions into the historical capability harnesses.

Run `python3 cache/key_layout_census.py --check cache/KEY_LAYOUT_V1_CENSUS.md` after changing
the layout or either checked-in measurement input.
