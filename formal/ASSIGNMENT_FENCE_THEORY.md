# Assignment-fence theory and refinement boundary

This document states the theory used by the executable assignment-fence gates.
It does not authorize product protocol behavior by itself.  The executable
matrix, TLAPS obligations, concrete C++ seam tests, mixed-version tests, and
performance gates must all agree with this boundary.

## 1. Roles, identity, and observables

The relevant roles are:

- `S`: scheduler;
- `F`: fulfillment daemon;
- `C`: client attached to `F`.

For a token-capable assignment, the authoritative identity is

```text
A = <scheduler_epoch, full_assignment_id, nonce>
```

and equality is component-wise.  A truncated wire identifier is never an
identity substitute.  `Legacy` and `FencedLegacy` retain their explicitly
weaker compatibility guarantees; only `Token` claims exact rejection of a
prior-epoch delayed claim.

The safety-relevant observable alphabet is:

```text
PrepareConsumed(A)
ReadyObserved(A)
UseCSExposed(A)
ClaimLinearized(A)
CompilerSideEffect(A)
Started(A)
Revoked(A)
Terminal(A, cause)
Released(A)
SessionLost(peer, generation)
```

Internal queue insertion, pending-table insertion, duplicate rejection, and
bookkeeping transitions are unobservable stuttering steps unless they emit one
of these events.

## 2. Ownership state machine

The logical assignment phases are:

```text
Requested
Prepared
Ready
UseCSExposed
PendingClaim
Claimed
Started
Revoked
Terminal
Released
```

Not every deployment mode exposes every phase as a wire message, but every
product transition must map to one logical phase and preserve the following
ownership facts.

### Core safety properties

For every assignment `A`:

```text
StartAuthorized(A) ==
    Started(A) => PrepareConsumed(A) /\ ClaimLinearized(A) /\ ~Revoked(A)

SideEffectAuthorized(A) ==
    CompilerSideEffect(A)
    => PrepareConsumed(A) /\ ClaimLinearized(A) /\ ~Revoked(A)

ClaimRevokeExclusion(A) ==
    Revoked(A) => ~ClaimLinearized(A) /\ ~Started(A)

TerminalUniqueness(A) ==
    Cardinality({cause : Terminal(A, cause)}) <= 1

ReleaseAfterTerminal(A) ==
    Released(A) => \E cause : Terminal(A, cause)

LiveIdentityUniqueness ==
    \A A1, A2 : Live(A1) /\ Live(A2) /\ SameFullIdentity(A1, A2)
                 => A1 = A2
```

Unknown or compacted assignment identities are rejected.  Compaction may
replace a live record only with a rejection summary that preserves the ability
to reject every delayed identity covered by the advertised compatibility
contract.

## 3. Ordered control relation

`PREPARE(A)` and `REVOKE(A)` use one ordered `S -> F` session stream.  `F` may
linearize a matching claim before it consumes a later queued `REVOKE(A)`, or
consume `REVOKE(A)` first and install the rejection fence.  The two outcomes
are intentionally different, but both are linearizable:

```text
Claim first:  ClaimLinearized(A); later REVOKE cannot revoke the started claim.
Fence first:  Revoked(A); every later matching claim is rejected.
```

A send or enqueue at `S` is not the fence linearization point.  The fence wins
only when `F` consumes the ordered revoke transition.  `S` releases ownership
only after consuming the corresponding terminal result, not when enqueueing
`REVOKE`.

## 4. Strict enforcing baseline

`StrictEnforcing` requires:

```text
UseCSExposed(A) => ReadyObserved(A)
ReadyObserved(A) => PrepareConsumed(A)
```

Thus the scheduler does not expose `UseCS` until the new fulfillment daemon has
consumed `PREPARE` and the scheduler has observed `READY`.

This mode has the smallest failure surface and no unmatched-claim table.  Its
cost is one additional control round trip on the assignment critical path.
It is the default design until measured latency or throughput evidence shows
that this round trip is unacceptable.

## 5. Pipelined enforcing contingency

`PipelinedEnforcing` may expose `UseCS(A)` after `PREPARE(A)` is enqueued but
before `F` consumes it.  It does **not** permit default allow.

A claim arriving before `PREPARE(A)` is consumed may only create

```text
Pending(A, exact_identity, client_session_generation)
```

in a bounded table.  It may not create a compiler process, environment,
filesystem mutation, reservation transfer, monitor `Begin`, or any other
external side effect.

The required invariant is:

```text
PendingNoSideEffect(A) ==
    Pending(A) /\ ~PrepareConsumed(A)
    => ~CompilerSideEffect(A) /\ ~Started(A)
```

When `F` later consumes `PREPARE(A)`, it may match the exact pending identity
and linearize the claim.  When `F` consumes `REVOKE(A)` first, it discards or
rejects the pending identity and emits the terminal fence result.  Unknown
identities are rejected once the negotiated pending bound is reached.

The table has an explicit bound `MaxPending` and therefore:

```text
PendingBound == Cardinality(PendingTable) <= MaxPending
```

with `O(1)` expected lookup by exact token and `O(MaxPending)` memory.  No full
global scan is allowed on claim, prepare, revoke, completion, or disconnect.

## 6. Pipelined-to-strict refinement

Let `P` be a pipelined execution and `T` a strict execution.  Define the
abstraction `alpha` as follows:

- a pipelined `UseCS` exposure before prepare consumption is internal until it
  produces an authorized observable;
- a pending unmatched claim maps to the strict state in which the claim has not
  yet been delivered;
- consumption of `PREPARE` followed by exact pending match maps to strict
  `READY`, `UseCS`, and claim delivery, with internal stuttering permitted;
- `Started`, `Terminal`, `Released`, and `SessionLost` map identically.

For the safety alphabet

```text
{CompilerSideEffect, Started, Revoked, Terminal, Released, SessionLost}
```

`PipelinedEnforcing` must stutter-refine `StrictEnforcing`:

```text
Trace(alpha(P)) | safety_alphabet
    in Traces(StrictEnforcing) | safety_alphabet
```

This refinement requires all of the following premises:

1. `PREPARE` and `REVOKE` share one ordered live session stream.
2. Pending lookup uses the exact assignment identity and client-session
   generation.
3. No side effect occurs before prepare consumption and exact match.
4. The pending table is bounded and overflow rejects rather than default-allows.
5. Session loss invalidates unmatched pending records from that generation.
6. Compaction retains a rejection summary adequate for the advertised delayed
   claim bound.

Removing any premise is a direct mutant obligation, not an implementation
choice.

## 7. Finite F-to-S ordering quotient

The concrete diagnostic model originally carried absolute sequence values:

```text
Qc = << <m1,n1>, <m2,n2>, ..., <mk,nk> >>
```

where `n1 < n2 < ... < nk`, and incremented a global `nextF2SSeq` across reset
cycles.  Those magnitudes do not affect protocol behavior and make the model
state space unbounded.

The finite quotient is:

```text
alpha(Qc) = << m1, m2, ..., mk >>
```

with queue position as the ordering object.  Enqueue appends one payload;
ordinary dequeue consumes position 1.  The explicit bypass mutant consumes
position 2 while position 1 remains and records the bounded observation
`lastF2SIndex = 2`.

### Simulation argument

Assume the concrete queue sequence values are strictly increasing.

- Concrete enqueue of `<m,n>` at the tail corresponds to abstract append of
  `m`.
- Concrete FIFO dequeue of the least sequence value corresponds to abstract
  removal of position 1.
- Every protocol predicate used by the accepted fixed model depends on payload,
  assignment identity, queue membership, or relative FIFO order, not on the
  numeric magnitude of `n`.
- The bypass mutant remains discriminating because consuming position 2 sets
  `lastF2SIndex = 2`, violating `lastF2SIndex <= 1`.

Therefore the quotient is a forward simulation for the accepted safety and
reachability properties.  It intentionally does not preserve diagnostics that
ask for an absolute synthetic sequence number; no product contract uses such a
number.

## 8. Progress assumptions

No liveness theorem assumes compiler completion, peer survival, or network
repair.  Accepted progress statements name only the actions whose fairness is
required.

For a live ordered link:

```text
WF(consume queue head)
```

permits eventual processing of an already queued control frame.  For the
pipelined pending theorem, an already pending exact claim is eventually
resolved only while matching prepare/revoke consumption remains live and weakly
fair.  Connection loss is a separate explicit terminal outcome.

## 9. Compatibility policy

Policy is immutable per assignment:

```text
old F             -> Legacy
new F + old C     -> FencedLegacy
new F + new C     -> Token
```

`PREPARE`, `READY`, token fields, and new terminal shapes are never sent to an
old peer.  A new client assigned to an old fulfillment daemon uses the frozen
legacy projection.  A capability observed on one connection cannot change a
later assignment's policy.

`PipelinedEnforcing` is eligible only for a negotiated new-`S`/new-`F`/new-`C`
Token assignment.  It is not a cluster-global mode and is never inferred from
another peer's capability.

## 10. Acceptance consequences

The design is not complete until all of the following are green at one exact
clean revision:

1. the unbounded ownership/identity TLAPS theorem, with zero failed obligations;
2. the finite FIFO network matrix on both pinned TLC distributions;
3. strict and pipelined fixed modes, race witnesses, and premise mutants on
   both pinned TLC distributions;
4. normalized trace-to-harness validation for every expected counterexample;
5. real C++ old/current codec and mixed-process compatibility tests;
6. deterministic product seam tests for every emitted barrier;
7. throughput, assignment latency, scheduler CPU, RSS, descriptor, reconnect,
   and management-latency comparisons at small, medium, and sustained scale.

If strict mode meets the performance budget, pipelining is unnecessary and
should not land.  If strict mode misses the budget, pipelining may land only
when its refinement premises and bounded-resource gates are independently
green.
