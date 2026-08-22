# P49/P50 assignment ownership model

`Protocol50Assignment.tla` is the bounded formal gate for the next product
slice after the accepted P48 endpoint/core convergence.  It does not replace
`Protocol50.tla` or `Protocol50JobLifecycle.tla`; it composes the scheduler
assignment with the already-modeled compiler-attempt lifecycle.

## Ownership boundary

```text
scheduler assignment identity
    epoch + wire_id + assignment_nonce + chosen worker

worker assignment record
    RESERVED -> CLAIMED -> STARTED -> TERMINAL

compiler attempts
    zero, one, or more child attempts after one claim wins
```

A compiler-child restart is not another scheduler assignment and does not
repeat `ASSIGN_PREPARE`, `ASSIGN_READY`, `UseCS`, or the worker claim.  It is a
new compiler attempt under the already-authorized assignment.

## Four admission modes

```text
LEGACY
    no prepare table is required; unknown wire ids may claim the assignment
    currently occupying that id in the worker's scheduler epoch

ADVISORY
    prepare is sent but UseCS is not READY-gated; known records are checked,
    unknown claims remain compatibility-admissible

ENFORCING_COMPAT
    UseCS is READY-gated and unknown ids are rejected, but nonce-less old
    clients may still claim a prepared assignment by wire id

STRICT_NONCE
    UseCS is READY-gated and the worker accepts only a matching
    (epoch, wire_id, nonce) claim
```

Protocol 49 reserves all four values but can operate only `LEGACY`,
`ADVISORY`, and `ENFORCING_COMPAT`, because its client claim carries no nonce.
Selecting `STRICT_NONCE` on a protocol-49-only path must be refused.  Protocol
50 makes the already-modeled strict transition operational by carrying the
full assignment identity through `UseCS` and `CompileFile`.

The critical distinction is:

```text
ENFORCING_COMPAT proves "the worker prepared this wire id".
It does not prove that a nonce-less old client belongs to this generation.

STRICT_NONCE proves generation identity, because nonce-less claims are rejected.
```

`Protocol50AssignmentMixedCompat.cfg` is therefore an **expected
counterexample**, not a product mutant.  It demonstrates that an old published
UseCS from epoch E0 can present a reused wire id after epoch E1 is prepared in
ENFORCING_COMPAT mode.  The worker can attribute that claim only to the
current record.  Exactness in a mixed fleet is a property of nonce-bearing
claims, not of every assignment.

## Revocation vocabulary

The worker result is modeled as:

```text
REVOKED
    a same-epoch tombstone was installed before the acknowledgement;
    no claim won and none may later win while that tombstone remains

CLAIMED_OR_LATER
    a claim already won; the scheduler retains ownership
```

The second result deliberately includes a compiler that has not started yet.
Once the client claim is accepted, revoking the assignment would require a
separate client-visible cancellation policy.  Protocol 49 should not call this
state merely `STARTED` if the implementation also uses it for claimed or
completed work.

The scheduler may release a dispatched assignment only after a matching
`REVOKED` result.  A timeout or absent reply is not a release proof.

## Tombstone and epoch lifetime

For exact revocation, a tombstone must remain installed for the worker's entire
scheduler-session epoch.  An arbitrary TTL cannot prove safety against an
arbitrarily delayed client.  The compact first implementation should therefore
clear assignment records and tombstones only on:

```text
scheduler-link loss
or
verified scheduler-epoch replacement
```

Link loss also retires the current epoch locally.  The worker cannot configure
again until `RestartScheduler` installs a different, unused epoch.  Clearing
the table and then reopening the same epoch would discard the only revocation
proof and let a released assignment claim again.  The epoch-reuse mutant keeps
the old epoch across link loss and is required to violate
`SameEpochRevocationSafety`.

Accepted results and their per-assignment attempt counts outlive scheduler-link
loss together.  Resetting only the count would leave an accepted result without
the authorization history that justifies it.

Within one live epoch, boundedness comes from scheduler dispatch credit and the
finite wire-id/assignment population.  If product measurements later demand
TTL eviction, the guarantee must be restated as bounded-delay rather than
unconditional revocation safety.

Scheduler epochs are compared for equality only, but their values must not be
reused within the stale-message horizon.  A random token is a probabilistic
uniqueness mechanism, not a monotonicity proof.  The bounded model represents
this with `usedEpochs` and permits restart only to a not-yet-used epoch value.

## READY latency and credit

ENFORCING_COMPAT and STRICT_NONCE necessarily add the S -> F -> S READY round trip before
UseCS.  ADVISORY is the zero-added-latency compatibility mode but provides a
weaker claim guarantee.  The default operating mode should be selected from
physical p95/p99 dispatch measurements; the formal model does not hide that
latency tradeoff.

Dispatch credit should be debited when the scheduler commits to PREPARE, not
later at UseCS, so PREPARED assignments remain inside the existing outstanding
work bound.  A worker whose scheduler channel has a deferred-output backlog
should not receive more preparations until that backlog drains or fails.

## Compiler-child restart

The scoped `RestartSpec` starts after one strict, nonce-exact claim has already
been authorized and compiler attempt zero has failed.  Under weak fairness of
starting and finishing the replacement child:

```text
attempt one eventually finishes
without another claim
without another prepare/ready exchange
without another scheduler assignment
```

This composes with the retained-input contract:

```text
accepted worker claim
AND independently owned exact InputRecord
    -> replacement compiler child may start
```

A cache-service or compiler-child restart must not reuse one mutable read
offset; every compiler attempt needs an independent cursor or `pread()`-style
offset.

## Checked rows

The runner adds:

```text
PASS
    assignment safety
    compiler-child restart progress

EXPECTED COUNTEREXAMPLE
    ENFORCING_COMPAT does not make nonce-less claims generation-exact

EXPECTED MUTANT FAILURES
    STRICT_NONCE accepts a legacy claim        -> StrictClaimsExact
    UseCS is published before READY             -> ReadyGate
    scheduler releases without REVOKED proof    -> ReleaseHasRevocationProof
    prepare resurrects an epoch tombstone       -> TombstoneIsNotLive
    link loss reuses the retired scheduler epoch -> SameEpochRevocationSafety
```

## Deliberate exclusions

This model does not include:

```text
wire serialization
scheduler capacity arithmetic
network timing
reserved-entry leases
environment transfer
cache-source Protocol 50 transactions
P29 or GRZ
multi-F routing policy
```

Those remain implementation, farm, or existing formal-model concerns.  A new
state variable should be added here only when P49/P50 product code introduces a
new assignment owner or a transition that cannot refine the actions above.

## Verification status

No TLC success is claimed for a changed model until `make protocol50-formal`
is run from the exact branch head with a pinned `tla2tools.jar`, retained
module/config/log hashes, state counts, depth, runtime, and an independent
reproduction.  The fail-closed runner requires both positive rows to pass and
all six counterexample/mutant rows to fail through their named invariant.
