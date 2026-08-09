# Finite FIFO quotient for assignment fencing

## Problem

The original `AssignmentFenceNetwork` attached an ever-increasing natural
number to each F→S message and retained `nextF2SSeq` and
`lastF2SConsumed` in the model state.  Repeated claim/revoke cycles therefore
created infinitely many history-distinct states even when every live queue,
assignment, worker, protocol phase, and capacity value was bounded.

That history was not product state.  The transport guarantee used by the
protocol is FIFO order among **currently queued** frames.  Absolute sequence
magnitude after all earlier frames have left the queue is unobservable and has
no effect on any fixed transition.

## Abstraction

Let a concrete F→S queue be

```text
<< [kind |-> k1, assignment |-> a1, seq |-> n1],
   ...,
   [kind |-> km, assignment |-> am, seq |-> nm] >>
```

with `n1 < ... < nm`.  The quotient map is

```text
alpha(queue) = << [kind |-> k1, assignment |-> a1],
                  ...,
                  [kind |-> km, assignment |-> am] >>
```

and erases `nextF2SSeq` and `lastF2SConsumed`.  Queue position is the order.
A single monotone Boolean, `f2sBypassObserved`, records the only historical
fact required by the direct FIFO mutant: whether S ever selected an index
other than one.

For every fixed transition:

1. F enqueue appends the same finite `(kind, assignment)` record, so `alpha`
   commutes with append.
2. S consumption selects index one and removes that record, so `alpha`
   commutes with head consumption.
3. Every non-FIFO action leaves both queue order and the observer unchanged.
4. The isolated bypass mutant selects index two when available and sets
   `f2sBypassObserved`; the fixed model can never set it.

Thus every concrete fixed execution maps step-for-step to a quotient execution,
and every quotient fixed step is realized by any strictly increasing concrete
renumbering of its live queue.  Safety properties that do not inspect erased
sequence magnitude are preserved in both directions.

## Finite-state argument

`Assignments`, `Workers`, message kinds, phases, policies, link states, and all
queue bounds are finite in each TLC configuration.  After quotienting, each
transport record ranges over a finite product of message kind and assignment,
and each sequence has a configured finite maximum length.  The remaining
counters are bounded by the model or become terminal after one assignment
lifecycle.  The fixed network therefore has a finite reachable graph.

`QueueEntriesUnique` is checked as part of `SafetyInvariant`.  A live queue key
may be reused only after an observable state in which its prior instance is
absent.  This permits a finite per-frame liveness statement without restoring
an unbounded ticket history.

## Liveness correction

The earlier property required an entire queue eventually to become empty.
That is stronger than per-frame progress and is false under sustained legal
arrival: a head can be consumed while a new tail is appended before the queue
reaches length zero.

`NoPermanentQueuedFrame` now quantifies over finite queue keys.  Every present
kind/assignment frame must eventually become absent, or its corresponding link
must be lost.  For delayed claims the finite key is assignment plus exactness.
The fairness assumptions remain limited to live-link drain and protocol
processing actions; compiler completion and peer availability are not smuggled
into scheduler-controlled fairness.

## Direct mutant discipline

The FIFO mutant now has the minimal six-state witness:

```text
S queues PREPARE(a0)
S queues PREPARE(a1)
F queues READY(a0)
F queues READY(a1) behind READY(a0)
mutant S consumes READY(a1) while READY(a0) remains
```

The default-allow mutant checks `NoStartAfterRevokedCompaction`, not the generic
`NoStartAfterRelease`.  This prevents TLC from satisfying the row through the
shorter and unrelated session-loss path; the retained trace must contain
REVOKE, REVOKED consumption, terminal compaction, and the delayed legacy claim
starting afterward.

## Reproducible results

Pinned tools:

```text
TLA+ Tools 1.7.4
sha256 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88

TLA+ Tools 1.8.0
sha256 e22f8ffb4bacdea0a871f444dd94fe5fb0d8013b3388ae39e82e26f852c735d5
```

Both tools produced identical state counts and verdicts:

| Row | Result | Generated | Distinct | Depth |
| --- | ---: | ---: | ---: | ---: |
| network-fixed | pass | 68,237 | 17,582 | 31 |
| claim-wins queued-revoke witness | counterexample | 489 | 225 | 8 |
| fence-wins delayed-claim witness | counterexample | 879 | 384 | 9 |
| network-compaction-fixed | pass | 71,277 | 17,940 | 31 |
| UseCS-before-READY mutant | counterexample | 34 | 26 | 4 |
| release-on-enqueue mutant | counterexample | 67 | 41 | 5 |
| default-allow-after-compaction mutant | counterexample | 2,972 | 1,200 | 11 |
| F→S bypass mutant | counterexample | 108 | 58 | 6 |
| per-frame live-link liveness | pass | 68,237 | 17,582 | 31 |

The stable text traces and differential native JSON traces for all six expected
counterexamples normalize to the same essential event and harness sequences.
`assignment_fence_fifo_quotient_test.py` makes the abstraction and row wiring a
cheap fail-closed preflight.

## Boundary

This result closes the finite strict-network representation and its existing
race/mutant matrix.  It does not yet establish the separately negotiated
`PipelinedEnforcing` pending-claim design, the TLAPS ownership proof, production
message compatibility, C++ model-to-code replay, performance, or rollout.
