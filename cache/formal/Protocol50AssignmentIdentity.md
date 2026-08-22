# Protocol-50 end-to-end assignment identity

`Protocol50AssignmentIdentity.tla` is the bounded bridge from the accepted
P49 scheduler/worker assignment controls to the P50 client claim. It adds no
new assignment owner or product message.

The existing `job_id` remains the assignment `wire_id`. Protocol 50 carries
the two additional fields through the claimant path:

```text
Scheduler --UseCS(epoch, job_id, nonce)--> C
C --CompileFile(epoch, job_id, nonce)--> F
```

The accepted `Protocol50AssignmentDelivery.tla` remains authoritative for the
full `(epoch, wire_id, nonce)` on P49 scheduler/worker PREPARE, READY, REVOKE,
and REVOKE_RESULT. This companion model covers only propagation through the
client-visible UseCS and CompileFile claim.

## Identity shapes

There are exactly three relevant shapes:

```text
exact P50
    (epoch, wire_id, nonce)

whole legacy compatibility
    (absent, wire_id, absent)

partial -- always invalid
    (epoch, wire_id, absent)
    (absent, wire_id, nonce)
```

Protocols 43, 48, and 49 retain the established `job_id`/`wire_id` but omit
both P50 metadata fields on UseCS or CompileFile. A P50 hop preserves all
three fields. It never synthesizes one missing field or silently treats a
partial token as legacy.

## Admission modes

```text
EnforcingCompat
    accepts one exact token
    or one wholly absent legacy token
    rejects partial and stale tokens

StrictNonce
    explicit operator promise for an all-P50 remote claimant path
    accepts only the exact full token
    refuses mixed remote configuration
```

This is the existing distinction from `Protocol50Assignment.md` made
wire-realizable. EnforcingCompat proves a prepared wire-id assignment while
permitting a nonce-less legacy claimant; it does not turn that claimant into
generation-exact P50. StrictNonce makes the generation-exact promise only
after S, C, and F all negotiate P50.

## Explicit P43/P50 compatibility cells

P43 is the explicit representative for the shared P43/P48/P49 claimant-hop
behavior. The positive model additionally ranges over P48 and P49 and checks
that they omit both added components.

| S | C | F | F receives | EnforcingCompat | StrictNonce remote |
|---|---|---|---|---|---|
| P43 | P43 | P43 | whole legacy | accept legacy | refuse |
| P43 | P43 | P50 | whole legacy | accept legacy | refuse |
| P43 | P50 | P43 | whole legacy | accept legacy | refuse |
| P43 | P50 | P50 | whole legacy | accept legacy | refuse |
| P50 | P43 | P43 | whole legacy | accept legacy | refuse |
| P50 | P43 | P50 | whole legacy | accept legacy | refuse |
| P50 | P50 | P43 | whole legacy | accept legacy | refuse |
| P50 | P50 | P50 | exact full tuple | accept exact | accept exact |

Each row has its own deletion-sensitive reachability config. The runner
requires `NoExpectedCellOutcome` to fail only after the stated outcome is
reached. This makes a deleted propagation or compatibility transition visible
instead of letting an unreachable invariant pass vacuously.

## Local exemption

A scheduler local decision creates no remote fulfillment assignment, sends no
remote UseCS/CompileFile identity, and needs no revoke proof. The local witness
uses P50 S and C with a P43 F to show that the unused F capability is
irrelevant. This is an exemption from a remote claim, not a legacy remote
claim admitted under StrictNonce.

## Reconnect and settlement correspondence

A client transport reconnect preserves the immutable UseCS identity. The
reconnect witness reaches an exact P50 claim after reconnect; dropping the
token during that transition violates `ReconnectPreservesIdentity`.

The terminal suffix mirrors the accepted delivery model:

```text
matching Revoked before claim
    -> immediate assignment release

matching ClaimedOrLater
    -> retain ownership
    -> ordinary settlement
    -> release
```

The `ReleaseClaimed` mutant releases directly on `ClaimedOrLater` and must
violate `ClaimedResultRetains`. The pre-claim and ordinary-settlement witnesses
make both valid release paths reachable.

## Mutants and witnesses

The focused runner checks:

```text
PASS
    all P43/P48/P49/P50 combinations in both admission modes

EXPECTED REACHABILITY
    eight P43/P50 EnforcingCompat cells
    all-P50 StrictNonce exact claim
    mixed remote StrictNonce refusal
    local exemption
    exact claim after reconnect
    matching Revoked release
    ClaimedOrLater ordinary settlement

EXPECTED MUTANT FAILURE
    stale epoch, wire, or nonce accepted
    epoch-only or nonce-only identity accepted
    all-P50 propagation deleted
    StrictNonce accepts whole legacy absence
    reconnect drops the exact token
    ClaimedOrLater releases before ordinary settlement
```

Run with a pinned TLA+ tools jar:

```sh
TLA2TOOLS_JAR=/absolute/path/to/tla2tools.jar \
TLC_WORKERS=1 \
sh cache/formal/run_assignment_identity_tlc.sh
```

No success is claimed for a changed head until SANY, the full focused matrix,
and an extracted-distribution replay all pass with retained hashes and state
counts.
