# G4 scheduler-session / batch-ledger model

**Product and tester-ready base:** `d206c7a012b173bd5ad92b5da3ed5a92abcc316c`
**Formal branch:** `bigoracle/g4-formal-d206c7a`
**Status:** formal source under independent review; no TLC result is implied by this document.

## Boundary

The model covers the daemon-global scheduler session and the per-client GetCS ledger:

- protocol activation and scheduler generations;
- explicit count modes: count zero, scalar one, and batch N;
- remote, local, and NoCS decisions;
- one bound/active local lane **per client**;
- FIFO selection within a client's ledger;
- lexicographic `(niceness, client_id)` selection when capacity is delivered across clients;
- local capacity charge and release counters;
- semantic `JobBegin` outcomes: committed, no-commit failure, and ambiguous committed-frame failure;
- accepted, rejected, duplicate, and stale completion attempts;
- ordered semantic outputs (`UseCS`, `Begin`, `Done`, cleanup-by-job-id, and one optional tail cancellation), with exact metadata/identity correlation;
- normal client End, abnormal client End, and daemon-global scheduler loss as separate settlement causes;
- reconnect to a genuinely newer scheduler generation;
- completed-token and cancelled-request memory.

It deliberately does **not** model compiler-child ownership, compiler termination, or raw byte-prefix transport. The corresponding claims are outside `CoreSafety`. A product trace may classify a send as `Committed`, `NoCommit`, or `Ambiguous`, but the model does not infer that classification from a boolean write return.

## State-machine facts being tested

### NoCS is local-lane work

A batch NoCS decision is not terminal at acceptance. Its normal path is:

```text
Absent
  -> LocalWaiting
  -> LocalBound
  -> LocalDelivered        (one slot charged; UseCS emitted)
  -> LocalStarted          (semantic Begin committed or conservatively ambiguous)
  -> Terminal/Done         (one Done emitted; slot released once)
```

End or scheduler loss can instead settle any unfinished recorded entry by full job identity.

### Binding and delivery are different decisions

`BindLocal(c,d)` selects the first `LocalWaiting` decision for client `c`, and each client has at most one lane entry. Several clients may therefore be bound simultaneously.

`DeliverLocal(c,d)` consumes global capacity. Among bound clients it selects the minimum `(niceness, client_id)` pair. This distinction matches the product's per-client scalar lane and global local-capacity competition.

### Exact settlement shape

For each request entering `Settling`:

1. every recorded unfinished entry is emitted as one `CleanupJob` in ledger order;
2. if `accepted < expected`, exactly one `CancelTail` is emitted for the request identity;
3. the request closes only after those obligations are complete.

The model never synthesizes one cancellation for every unrecorded entry.

### Begin ordering

A local entry may be `LocalStarted` only when exactly one matching `Begin` semantic output exists. An ambiguous send is modeled conservatively as a committed Begin plus global scheduler loss. A no-commit failure produces no Begin and cannot enter `LocalStarted`.

## Safety conjunction

`CoreSafety` checks:

```text
TypeOK
ActivationEvidence
LegacyActivationEnabled
ModeShape
RequestAccounting
AbsentShape
PerClientLaneUnique
FIFOSelection
PrioritySelection
CleanupOrder
CapacityAndRelease
BeginEvidence
DeliveryEvidence
TerminalUniqueness
NoCSUsesLocalLane
CurrentGenerationAuthority
CompletedNeverLive
CompletedOutputEvidence
TailCancellationExact
IdleAndCleanShape
NormalCompletionAuthority
NoUnexpectedDeadlock
```

The terminal and capacity properties are not true merely by representation. Separate variables count charge attempts, releases, terminal attempts, terminal emissions, and the ordered output sequence. The mutant rows can therefore create a double release, duplicate terminal, duplicate tail cancellation, extra Begin, or reordered cleanup and obtain a real counterexample.

## Cutoff and deadlock interpretation

Finite exhaustive rows use bounded scheduler and request generations. The only accepted terminal deadlock is:

```tla
CutoffState ==
  /\ st.session = "Disconnected"
  /\ ~st.lossPending
  /\ st.generation = MaxGeneration
  /\ \A c \in Clients : st.requestPhase[c] = "Idle"
```

The checked invariant is:

```tla
NoUnexpectedDeadlock == CutoffState \/ ENABLED Next
```

TLC's built-in deadlock check is disabled in the generated config only because this explicit invariant classifies the intended cutoff. The `MutantNonCutoffDeadlock` row disables the final cleanup action and must violate `NoUnexpectedDeadlock` outside `CutoffState`.

## Fairness

`FairSpec` adds weak fairness only for daemon-owned handling and cleanup actions. It does not assume:

- ConfCS arrival;
- network repair;
- compiler or worker completion;
- a client eventually sending End;
- the scheduler eventually assigning work.

`LossCleanupProgress` is conditional on entering the modeled loss-cleanup path and on daemon-owned fairness.

## Falsification plan

The model is treated as a theory to attack. One-premise mutants separately alter:

- legacy activation;
- batch bound enforcement;
- NoCS lane placement;
- per-client lane uniqueness;
- FIFO binding;
- cross-client priority delivery;
- terminal uniqueness;
- release uniqueness;
- tail-cancellation uniqueness;
- stale-generation rejection;
- Begin-before-start ordering;
- cleanup order;
- strict capacity comparison;
- cutoff/deadlock classification;
- whole-daemon loss cleanup;
- Begin output multiplicity.

A mutant is useful only when TLC reaches a nonzero state space and reports the intended named invariant, not a parser error, unrelated type error, or generic process failure.

## Verification ladder

1. Exactly 16 trace-checker controls pass locally.
2. SANY succeeds with the pinned 1.7.4 jar.
3. `fixed-modern-c1d1-cap1` exhausts a nonzero state space with queue-at-end zero and `CoreSafety` intact.
4. The remaining small fixed rows and each one-premise mutant run on 1.7.4.
5. Accepted rows are repeated with the pinned `2026.07.31` prerelease jar and compared.
6. Larger symmetry-reduced rows and seeded simulations cover more clients and decisions.
7. A separate inductive/TLAPS argument generalizes safety beyond the finite cutoffs.
8. Product transition logs from every deterministic integration scenario replay through the same action registry.
9. The seeded `1 scheduler / 6 fulfillment daemons / 31 clients` exercise remains a large execution gate, not a substitute for the inductive proof.


## Adversarial local printouts

`make formal_falsify` constructs deliberately invalid projected transitions and must reject all of them with named diagnostics. The current set attacks start-before-Begin, immediate-terminal NoCS, a second local lane, FIFO and priority inversion, stale mutation, duplicate terminal, double release, duplicate tail cancellation, cleanup order, missing remote UseCS, completed-token resurrection, output metadata mismatch, and ambiguous Begin misclassified as no-commit. These are conformance-level falsification controls; the corresponding TLA mutants are run only after SANY and the first fixed TLC row pass.
