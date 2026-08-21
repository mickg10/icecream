# P29 current-TU information boundary

## Contract

For translation unit `t`, the product codec is a transition over only:

```text
TU_t + committed global C state + committed selected-F route state
    -> physical TU transaction + next committed state
```

The complete current TU may be inspected. No later TU, final manifest size, final object
count, route suffix, or unfinished transaction state is available.

The complete design ruling and audit are recorded in Issue #16 comment 5334443182.

## First implemented block: online S1

`p29_online_s1.h` extracts P29's S1 longest-previous-factor stage into an object whose only
input method admits one complete current TU. It provides:

- native typed Region/Block references, with no `final_NREG + block_id` namespace;
- persistent global occurrence, hash-chain, and canonical Block state;
- deferred cross-TU boundary anchors;
- canonical Block children independent of any selected F's residency;
- metadata that distinguishes a source preceding the current TU without claiming that the
  selected F owns it.

The old full-route loop inserts anchors at the end of a TU whenever
`position + min_match <= final_occurrence_count`. Its last anchors can therefore read the
first Regions of the next TU. The online implementation obtains the same result without that
read: it retains the incomplete tail, and installs the newly complete boundary anchors only
after the next TU becomes the current input.

`p29_online_s1_test.cpp` contains a direct typed-reference translation of the old full-route
algorithm and compares every Root, new-Block record, and canonical Block against the online
implementation. The corpus includes:

- a forced useful match whose source k-gram crosses a TU boundary;
- empty and one-Region TUs that defer an anchor across multiple admissions;
- 200 deterministic mixed/repeated TUs;
- two different future suffixes after an identical prefix.

Run:

```sh
g++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Werror \
  capability/grouprlz/p29_online_s1_test.cpp -o /tmp/p29_online_s1_test
/tmp/p29_online_s1_test
```

Expected:

```text
P29 online S1 T_current/full-route equivalence PASS
```

The same test passes with AddressSanitizer and UndefinedBehaviorSanitizer.

## Still required before P29 is current-TU complete

1. Move `Interner::process` from the whole-manifest prepass into current-TU admission.
2. Replace the literal dump/plan second pass with direct per-TU `residual_group::Codec`
   encoding. For one-TU groups this is expected to be bit-identical because each existing
   group already uses a fresh codec and only that group's bytes.
3. Integrate `OnlineS1` into the P29 producer and compare actual Root/Block frames against
   stable-tag one-TU reference frames.
4. Split one global C object store from per-F known-object, occurrence, COPY/view, and entropy
   state.
5. Add the stream-only F receiver, an always-present Need result, relationship open/close,
   final identity layout, and transaction acknowledgement.
6. Pass the unavailable-suffix producer barrier and the complete multi-F M5 routing matrix.

The committed `dca69f31` source also includes three local headers that are absent from that
commit: `alpha_line_codec.h`, `mo_factor_codec.h`, and `residual_group_codec.h`. A clean checkout
cannot reproduce its documented build until those exact dependencies are committed alongside
the source.

## Shared BlockCatalogue (extraction on implementer/issue16-capability)

`OnlineS1` originally owned `blocks_` and `block_index_`, and `intern_block()` read the
matcher's own `occurrences_`. That makes "one global catalogue + one matcher per route"
unreachable by instantiation: a second matcher gets a second Block id space, so identical
content mints different ids and a Block imported on one route cannot be referenced on
another.

`BlockCatalogue` now owns the canonical children, the lookup index and the ids, and takes
children as a span. `OnlineS1(Config, BlockCatalogue&)` shares one; `OnlineS1(Config)` owns
a private one, which is the single-matcher case the equivalence test exercises. Those are
deliberately NOT delegating constructors — delegation initialises every member of the
target, including `owned_`, which destroys the just-created catalogue and leaves the
pointer dangling. That bug segfaulted the unmodified equivalence test on the first attempt.

`BlockUse` is recorded for EVERY Block use, not only when the catalogue mints an id, and
carries a matcher-local `source_position`. Identity is global; coordinates are not. A
source coordinate from the global matcher is not evidence that a selected F holds that
material, which is what makes per-F COPY legality a separate question.

### Gates

1. `p29_online_s1_test.cpp`, unmodified, still PASS (opt `-Werror` and ASan+UBSan) — it is
   the guard that the extraction is behaviour-preserving, so it is deliberately untouched.
2-5. `p29_block_catalogue_test.cpp`: same children → same id across matchers; source
   positions matcher-local and differing; a sequence only route A has admitted is not a
   route-B match until B admits it; the catalogue does not grow when a second or third
   matcher interns identical children.
6. The deferred-boundary mutations still fail (drop the boundary-anchor install; restore
   the `j+MINMATCH<=NS` future read).

Mutation-tested, because gates that cannot fail prove nothing:

| mutation | caught by |
|---|---|
| each matcher gets its own catalogue | gate 5 |
| report a global coordinate instead of the matcher-local one | gate 3 |
| record a use only when the id is new | gates 4/5 (no use recorded) |

**Gate 2 alone does not catch separate catalogues.** Two fresh catalogues assign ids in the
same order, so the ids coincide and gate 2 passes; only gate 5 — "the second matcher must
not mint" — detects it. Gate 2 is necessary but not sufficient, and the pair is what makes
the property sound.
