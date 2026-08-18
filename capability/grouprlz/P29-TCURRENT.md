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
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
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
