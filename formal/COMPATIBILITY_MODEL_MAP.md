# Mixed-version compatibility model-to-product map

The formal model uses symbolic versions and policies. Protocol numbers are
allocated only after rebasing the minimal upstream series. The product mapping
is per link and per assignment; no scheduler-wide capability bit may change an
assignment already evaluated against its actual client and worker generations.

## Roles and topology notation

```text
S'  new scheduler
F   old fulfillment daemon / compile server
F'  new fulfillment daemon
C   old compiler client through its local daemon
C'  new compiler client through its local daemon
```

The executable topologies are:

```text
S'FC
S'FC'
S'F'C
S'F'C'
S'F[F']C[C']
S'F'[CC']
```

## Per-assignment policy selection

| Formal policy | Negotiated links | Product behavior | Guarantee boundary |
|---|---|---|---|
| `Legacy` | old F with C or C' | frozen old messages and fields only; no PREPARE/READY/REVOKE wait | legacy within-session behavior; no arbitrary-delay restart exactness |
| `FencedLegacy` | new F with old C | worker assignment table and revoke fence; old client receives no full-id/token field | exact worker linearization within the live scheduler/worker epoch; old-client restart ABA remains a stated limitation |
| `Token` | new F with new C | full assignment identity and nonce in the first identity-bearing claim and terminal path | exact stale-claim rejection across scheduler restart |

A new client assigned to an old worker must project to `Legacy`. A prior Token
assignment must not make a later old-worker assignment Token-capable.

## Formal actions to product seams

| Formal transition | Product seam | Required executable evidence |
|---|---|---|
| `Request(c)` | scheduler request admission from the submitter daemon | negotiated client-channel version printed from the actual channel |
| `Assign(w)` | worker selection plus immutable assignment-policy choice | chosen worker and both negotiated versions captured before `UseCS` exposure |
| `ChoosePolicyLate` mutant | any code that derives capability after worker dispatch | mutation must show `DispatchedUndecided`; fixed code has no such state |
| `DeliverUseCS` | scheduler→submitter-daemon assignment and local client handoff | old projection consumes the complete old frame; new fields only on a negotiated new client link |
| `ClaimAtWorker` | first identity-bearing client→worker message | C uses wire ID; C' uses full ID + nonce; worker validates policy recorded by PREPARE/assignment table |
| `CancelBeforeDelivery` | exact cancellation before complete client handoff | one terminal result; no invented claim identity; old trace remains frozen |
| `RevokeBeforeClaim` | new-worker fencing before the client claim | scheduler releases only after complete worker result consumption; old client sees only old projection |
| `RestartScheduler` / `DelayedClaimAfterRestart` | scheduler epoch change and delayed client arrival | Legacy/FencedLegacy limitation tests remain explicit; Token claim from prior epoch is rejected |
| `WorkerSessionLoss` | assigned worker-generation teardown | exact one terminal result and no old-session capacity reuse before quiescence |
| `SubmitterLossBeforeStart` | live submitter-generation loss before Start | bounded terminal cleanup, exact accounting, complete management responses |
| `DetachStartedSubmitter` / `CompleteDetached` | STARTED job submitter detach and assigned-worker completion | worker remains terminal authority; stable detached identity remains observable |
| `CompactTerminalRecord` | bounded terminal/tombstone state compaction | later unknown claim remains rejected; no default-allow fallback |
| `ResetAssignment` | next request in the same scheduler process | monotonic observer only; no assignment state or capability leaks across reset |

## Frozen old projection

The old observable trace language is deliberately small:

```text
REQUEST ASSIGN USECS CLAIM BEGIN DONE
REQUEST ASSIGN CANCEL
REQUEST ASSIGN LOSS
REQUEST ASSIGN USECS CANCEL|LOSS
REQUEST ASSIGN USECS CLAIM LOSS
REQUEST ASSIGN USECS CLAIM BEGIN LOSS
```

PREPARE, READY, REVOKE, full assignment identity, nonce, and new terminal fields
are erased only because the old peer never receives them. Projection is not a
license to send a larger frame and ignore its suffix.

`compatibility_codec_fixture.py` checks a generated length-delimited envelope:

- frozen old assignment and terminal bytes;
- new-client Legacy projection byte-identical to the old encoder;
- exact complete-frame decoding;
- rejection of partial prefixes, partial bodies, and trailing bytes;
- one-frame-at-a-time stream consumption; and
- rejection of new assignment/terminal shapes by the old decoder.

This Python fixture is regeneration and discrimination evidence. Before an
upstream behavior PR is accepted, the same obligations must be mapped to the
real C++ message classes and exercised through a socketpair using binaries
built from the exact old and candidate revisions.

## Concrete mixed-version execution matrix

For every applicable topology run:

```text
normal completion
cancel before UseCS
revoke after UseCS but before claim (new F only)
submitter loss before Start
submitter detach after Start followed by worker Done
worker loss before and after Begin
scheduler restart with delayed claim
terminal-record compaction with delayed claim
```

The report must include actual negotiated versions from each channel, message
and field inventory observed by each old peer, exact terminal count, residual
job/reservation/debit counts, process exits, complete management terminators,
and retained packet/fixture logs.

## Rollout consequence

A new scheduler may be deployed first because every assignment is classified
from its real peer links. New workers can then be enabled in observe-only mode,
followed by enforcing fencing for assignments whose worker capability was
negotiated. Token enforcement is enabled only for new-client assignments. Old
clients and old workers retain the frozen projection throughout the rollout.
