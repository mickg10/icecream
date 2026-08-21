# Real C++ codec and mixed-binary acceptance gate

The generated Python compatibility fixture is a deterministic companion, not a
substitute for Icecream's real message classes, channels, binaries, or process
lifecycle.  This gate is mandatory before any product protocol behavior is
accepted.

## 1. Immutable inputs

A run records and verifies:

```text
old source revision
candidate source revision
compiler and linker versions
configure/CMake arguments
C/C++ flags and feature macros
old scheduler/daemon/client binary SHA-256 values
candidate scheduler/daemon/client binary SHA-256 values
real codec-test binary SHA-256 values
host kernel and architecture
```

The old revision is an exact commit or release tag resolved to an exact commit.
A moving branch name is not evidence.  Old and candidate builds use the same
compiler toolchain and semantically equivalent build options.

## 2. Test construction

The codec fixture is C++ and links the real communication/message
implementation.  Where old and candidate symbols cannot coexist in one
process, use two small helper executables connected by a Unix `socketpair` or
an inherited local stream descriptor:

```text
old-codec-peer
candidate-codec-peer
codec-controller
```

Each helper exposes only these operations:

```text
encode one named real message to the supplied descriptor
consume exactly one named real message from the supplied descriptor
report bytes consumed, decoded fields, protocol version, and terminal status
```

No helper reimplements a wire encoder or decoder.  It calls the production
message classes and channel framing functions.

## 3. Frozen old assignment bytes

For each old assignment shape used by the selected baseline:

1. old encoder writes one complete frame;
2. candidate encoder, forced to the old negotiated version and `Legacy`
   projection, writes the corresponding frame;
3. the byte strings must be identical in length and content;
4. old decoder consumes both frames and reports identical fields;
5. candidate old-version decoder consumes both frames and reports identical
   fields;
6. each decoder reports exactly one complete frame consumed and leaves the
   following sentinel frame untouched.

A semantic field comparison without byte equality is insufficient for the
frozen old projection.

## 4. New assignment identity bytes

For a negotiated new scheduler, new fulfillment daemon, and new client:

- encode/decode full scheduler epoch, full assignment identifier, nonce, policy,
  and client-session generation;
- test minimum, ordinary, and maximum representable non-reserved values;
- reject truncation, overflow, duplicate fields, and incompatible policy/field
  combinations;
- prove that changing any identity component changes the decoded identity;
- prove that a prior Token assignment cannot affect a later Legacy projection.

An old encoder is not asked to produce the new shape.  An old decoder must
reject or version-gate it at the documented framing boundary without silently
accepting a prefix.

## 5. Control and terminal vocabulary

Real C++ round trips are required for every new-peer control shape:

```text
PREPARE(A)
READY(A)
REVOKE(A, cause)
TERMINAL(A, cause, generation)
```

For an old peer, candidate code must neither send nor wait for these shapes.
The old protocol continues directly through its frozen assignment vocabulary.

The following incompatible combinations must be rejected before state-machine
side effects:

- `PREPARE`, `READY`, or `REVOKE` on an old-negotiated link;
- Token fields addressed to an old client;
- missing or zero reserved identity components;
- terminal shape whose policy disagrees with the assignment policy;
- control frame from a stale connection generation;
- duplicate terminal token for an already terminal identity.

## 6. Stream-boundary matrix

For every old and new frame shape, run all of these stream tests against the
real decoder:

1. empty input;
2. every incomplete framing-prefix cut;
3. every incomplete payload cut;
4. complete frame split at every byte boundary across two writes;
5. two complete frames in one write;
6. one complete frame plus trailing garbage;
7. declared length below minimum;
8. declared length above negotiated maximum;
9. peer closes before frame completion;
10. peer closes immediately after one complete frame.

A decoder pass requires exact consumed-byte count, exact next-frame boundary,
no uninitialized fields, no process crash, no busy loop, and a bounded failure
return.

## 7. Cross-version codec matrix

Execute each applicable direction:

| Encoder | Decoder | Negotiated shape | Expected result |
|---|---|---|---|
| old | old | old | pass |
| old | candidate | old | pass |
| candidate | old | old/Legacy projection | pass, byte-identical |
| candidate | candidate | old | pass, frozen bytes |
| candidate | candidate | new | pass, exact full identity |
| candidate-new-shape | old | incompatible | explicit reject, no desync |

The controller records raw frame SHA-256, byte count, consumed count, decoded
JSON, process exits, and the first mismatch offset.

## 8. Mixed-process topology matrix

After the codec matrix is green, execute real processes in these deployments:

```text
S'FC
S'FC'
S'F'C
S'F'C'
S'F[F']C[C']
S'F'[CC']
```

Here `S'` is the candidate scheduler, unprimed peers are exact old binaries,
and primed peers are candidate binaries.

### Required policy observations

```text
old F             -> Legacy
new F + old C     -> FencedLegacy
new F + new C     -> Token
```

For every assignment, retain the selected policy and the exact negotiated
version of both links.  A policy change after dispatch or capability leakage
from another assignment is an immediate failure.

## 9. Required lifecycle scenarios per topology

Run every applicable scenario with deterministic barriers:

1. normal request, assignment, Begin, completion;
2. cancellation before UseCS delivery;
3. claim consumed before a queued REVOKE;
4. REVOKE consumed before a delayed claim;
5. submitter disconnect before Begin;
6. submitter disconnect after Begin, then worker completion;
7. submitter disconnect after Begin, then worker disconnect;
8. scheduler restart with one retained delayed claim;
9. fulfillment-daemon reconnect/relogin generation change;
10. client reconnect generation change;
11. duplicate Begin;
12. duplicate completion/terminal;
13. partial UseCS at every deterministic byte cut;
14. terminal-record compaction followed by delayed claim;
15. management listjobs/listcs query at each barrier.

The harness must require complete successful management replies and a live
scheduler.  EOF, timeout, malformed row, or interrupted response is a test
failure and is never translated into zero jobs or absence.

## 10. Restart guarantee boundary

Expected outcomes remain mode-specific:

- `Legacy`: frozen behavior; arbitrary delayed old-client claim after scheduler
  restart remains an explicit limitation witness;
- `FencedLegacy`: exact worker-side fencing in the live epoch; old-client
  restart identity remains an explicit limitation witness;
- `Token`: exact stale prior-epoch identity rejection.

The two compatibility limitations are retained as positive expected witnesses.
They are not converted into passing exactness claims.

## 11. Strict and pipelined modes

`StrictEnforcing` is the first product implementation and gate:

```text
PREPARE consumed -> READY observed -> UseCS exposed
```

The real process trace must prove this order for every Token assignment.

`PipelinedEnforcing` is optional and runs only after strict performance
measurement.  Its real gate must show:

- UseCS may be exposed after PREPARE enqueue;
- a claim arriving first creates only a bounded pending record;
- no compiler/environment side effect precedes PREPARE consumption and exact
  match;
- pending overflow rejects;
- connection-generation loss invalidates pending entries;
- queued REVOKE and claim outcomes match the formal linearization cases.

## 12. Retained result format

Each run produces one immutable directory containing:

```text
inputs.json
build-old.log
build-candidate.log
binary-sha256.txt
codec-cases.jsonl
raw-frames/
process-topologies.jsonl
barrier-events.jsonl
management-replies.jsonl
process-exits.json
resource-samples.csv
result.json
```

`result.json` names the exact first failure and must not report green when any
required case was skipped.  A top-level workflow summary without these files is
not acceptance evidence.

## 13. Exit criterion

The real codec/mixed-binary gate is green only when:

- every frozen old byte comparison passes;
- every new exact-identity round trip passes;
- every partial/trailing/incompatible stream case has the required bounded
  outcome;
- every topology selects the required immutable policy;
- every required lifecycle scenario reaches its named barrier and outcome;
- all expected limitation witnesses remain present;
- every process exits as expected and the scheduler is live until requested
  shutdown;
- the checkout and generated evidence identities are exact and complete.

Formal proof, generated fixtures, source inspection, or a homogeneous
candidate-only process run cannot substitute for this gate.
