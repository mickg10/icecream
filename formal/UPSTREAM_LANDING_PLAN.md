# Upstream landing plan

The proof branch is an evidence and design branch.  It must not be merged into
upstream Icecream as one historical stack.  Product work is split into small,
bisectable changes whose tests cite the modeled transition they implement.

## Global rules

Every product PR must:

1. be based on the current upstream target, not on the evidence branch history;
2. contain one coherent behavior delta;
3. preserve old-peer wire bytes unless the peer negotiated the new version;
4. include a direct red/green regression for every changed transition;
5. contain no generated TLC/TLAPS run artifacts in the source tree;
6. link the exact immutable evidence SHA and retained workflow artifacts;
7. state runtime and memory complexity for each normal-path operation;
8. leave the tree buildable and deployable when merged alone.

No product PR may claim that opening, compiling, or statically checking a formal
model is a proof result.  Proof and model-check results require the exact clean
execution records defined by the focused generation-4 gate.

## PR A — protocol-neutral prerequisite corrections

Scope only the already identified non-protocol prerequisites:

- stable scheduler-visible full job identity allocation;
- STARTED-job terminal authority and submitter-detach ownership;
- exact UseCS failure settlement at every concrete byte boundary;
- old fulfillment-session compiler/process-group quiescence;
- existing connectivity/relisten/lifecycle corrections required by issue #4.

This PR adds no `PREPARE`, `READY`, or `REVOKE` behavior.  It must map to the
accepted 24-row prerequisite matrix and retain the existing old protocol.

## PR B — negotiated wire vocabulary and frozen old projection

Introduce only message definitions, feature negotiation, codecs, and unit
fixtures:

- full assignment identity and nonce fields for new peers;
- version-gated `PREPARE`, `READY`, `REVOKE`, and terminal-token shapes;
- immutable per-link negotiated capabilities;
- byte-identical old assignment and terminal encodings;
- exact old/new decoder rejection of incomplete, trailing, or incompatible
  frames.

No scheduler or daemon state-machine behavior changes in this PR.  Tests use
socketpairs and the real message classes.  The mixed-version matrix must build
an exact old baseline and candidate binary and retain both binary hashes.

Normal encode/decode cost remains `O(frame_size)` with bounded frame size and no
global state lookup.

## PR C — scheduler assignment ownership and strict enforcing mode

Implement the scheduler side of `StrictEnforcing` only:

- choose immutable policy per assignment before dispatch;
- create exact identity before any client-facing exposure;
- enqueue `PREPARE` only on a negotiated new-F link;
- wait for matching `READY` before exposing UseCS;
- enqueue ordered `REVOKE` on pre-start cancellation;
- release ownership only after consuming one terminal token;
- retain rejection identity until the advertised delayed-message bound is
  discharged.

Old F uses `Legacy`; new F plus old C uses `FencedLegacy`; new F plus new C uses
`Token`.  No capability observed on another assignment or connection may
change this choice.

Normal assignment, ready, revoke, and terminal lookups must be expected `O(1)`
by full identity.  No normal-path full job or daemon scan is permitted.

## PR D — fulfillment-daemon strict enforcement

Implement only the F-side strict state machine:

- consume exact `PREPARE` and install the assignment identity;
- emit `READY` only after installation;
- accept a claim only for the exact installed identity and live client session;
- make `Claim` versus ordered `REVOKE` consumption the linearization race;
- prohibit compiler/environment side effects before claim authorization;
- emit exactly one terminal token;
- invalidate assignment state on scheduler/client generation loss;
- retain or compact rejection state without default allow.

The PR contains deterministic barriers corresponding to every strict-mode
formal witness and mutant.  The default for an unknown or compacted identity is
reject.

## PR E — optional pipelined enforcing contingency

This PR does not exist unless the accepted cluster comparison shows that strict
READY-before-UseCS misses the agreed latency or throughput budget.

When justified, it adds only:

- a separately negotiated `PipelinedEnforcing` capability;
- a bounded exact-identity pending-claim table;
- no-side-effect-before-PREPARE enforcement;
- ordered PREPARE/REVOKE resolution of pending claims;
- overflow rejection, generation invalidation, metrics, and tests.

It must satisfy the stuttering-refinement premises in
`ASSIGNMENT_FENCE_THEORY.md`.  Pipelining is never inferred from cluster-global
capability and is never sent to an old peer.

Expected lookup is `O(1)`; memory is `O(MaxPending)` with a configured and
observed hard bound.  Failure to meet the bound is a correctness failure, not a
performance warning.

## PR F — mixed-process, scale, and rollout gates

Land testing and rollout support that is useful independently of one release:

- exact old/current codec socketpair matrix;
- S′FC, S′FC′, S′F′C, S′F′C′, S′F[F′]C[C′], and S′F′[CC′] process rows;
- deterministic disconnect/reconnect/restart and delayed-message barriers;
- scheduler management-latency and resource metrics;
- 10/25/50-F cluster drivers with 100/1,000/5,000/10,000+ C/C′ tiers;
- feature-off, strict canary, mixed canary, and rollback controls.

Rollout is disabled by default.  Enabling requires exact evidence identities,
not branch names.  A rollback disables new assignment policy selection; it does
not reinterpret already-issued identities.

## Review order and merge dependency

```text
PR A
  -> PR B
       -> PR C
            -> PR D
                 -> PR F
                      -> PR E only if strict misses the measured budget
```

PR E is intentionally last and optional.  Correctness is not traded for a
hypothetical hot-path benefit.  If strict mode meets the target, the smaller
strict protocol is the final design.

## Evidence cited by each PR

Each PR description includes:

```text
formal evidence revision
workflow run IDs
artifact IDs and SHA-256 digests
TLC versions and jar SHA-256 values
TLAPM distribution and backend identities
per-row generated/distinct states, depth, elapsed time, peak RSS
trace and harness hashes
old/candidate binary SHA-256 values
cluster inventory and result SHA-256 values
first retained failure, if any
```

Mutable branch names, screenshots, abbreviated success summaries, and source
publication alone are not acceptance evidence.
