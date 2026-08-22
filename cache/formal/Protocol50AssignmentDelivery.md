# Protocol-50 assignment delivery refinement

`Protocol50Assignment.tla` models assignment ownership by logical assignment.
This companion model checks whether the proposed wire carries enough identity
to refine those assignment-keyed actions when one scheduler job/wire id is
assigned more than once in one scheduler epoch.

## Wire ruling

The complete assignment identity is:

```text
(epoch, wire_id, assignment_nonce)
```

It must be present on all assignment-control messages:

```text
ASSIGN_PREPARE
ASSIGN_READY
REVOKE_BEFORE_START
REVOKE_RESULT
```

`ASSIGN_READY(epoch, wire_id)` is insufficient. A delayed READY for an older
nonce can be consumed after the scheduler creates a newer assignment with the
same job id, causing UseCS to be published before the newer reservation exists.

Likewise, nonce-less REVOKE requests or results can revoke or release the newer
assignment after a same-wire reassignment. TCP FIFO does not solve this:
requests and responses travel in opposite directions, and scheduler state may
change while an older response remains unread.

The added eight bytes per control message avoid imposing a scheduler rule that
a job id may never return to one worker during an epoch.

## Advisory claim before PREPARE

In ADVISORY mode an unknown legacy claim is permitted. The following order is
therefore valid:

```text
UseCS / CompileFile claim arrives
worker records CLAIMED
ASSIGN_PREPARE arrives later
```

The late PREPARE validates/enriches the existing claim and may produce READY,
but it cannot change the phase from CLAIMED back to RESERVED. A subsequent
REVOKE must still return `CLAIMED_OR_LATER`.

The revoke decision belongs to the worker's single assignment-record owner and
is made when that owner consumes the request, not when the scheduler queues it.
`Absent` or `Reserved` may advance to `Revoked` and return `Revoked` as a
positive pre-claim proof. `Claimed`, `Started`, or `Terminal` is preserved and
returns `ClaimedOrLater`. The scheduler releases immediately only for the
matching `Revoked` result. A matching `ClaimedOrLater` result retains ownership
until the ordinary terminal settlement.

This distinction covers the positive crossing

```text
SendRevoke -> AdvisoryClaim -> DeliverRevokeRequest
```

where the claim wins even though the revoke was queued first. It aligns this
focused delivery refinement with `Protocol50AssignmentOrdering.tla` and the
product behavior at `e05f6d0a`.

## Checked invariants

```text
PublishedHasMatchingPrepare
    UseCS publication is backed by READY for that exact assignment.

RevocationTargetsSentAssignment
    worker revocation or claimed/later proof applies to the assignment named
    by S.

RevokeResultHasMatchingWorkerProof
    scheduler release/retention uses the exact assignment and result proved by
    F.

ReleaseHasMatchingSettlement
    immediate release is backed by Revoked; a claimed assignment is released
    only by ordinary settlement.

RevokedHasNoClaimEvidence
    a claim observed before revoke delivery can never become Revoked.

ClaimedOrLaterPreservesWorkerState / CrossedClaimWinsRevoke
    revoke delivery preserves Claimed, Started, or Terminal, including the
    send-before-claim crossing.

StaleNonceResultDoesNotReleaseCurrent
    a delayed result for the old nonce cannot release the same-wire current
    assignment.

ClaimedPhaseNeverRegresses
    late PREPARE cannot move an accepted advisory claim backward.
```

Two reachability configurations require TLC to exhibit the send-before-claim
crossing through `ClaimedOrLater` retention and a delayed old-nonce `Revoked`
result that releases only the old assignment. They fail their intentionally
negated witness predicates only when the discriminating states are reached.

## Mutants

The fail-closed runner requires these four unsafe variants to violate their
named invariants:

```text
READY resolved by current wire id rather than nonce
REVOKE request resolved by current wire id rather than nonce
REVOKE result resolved by current wire id rather than nonce
late advisory PREPARE regresses CLAIMED to RESERVED
```

Every variant still transports the complete assignment identity. The three
wire mutants change only the record selected at delivery, deliberately
modeling an implementation that ignores the transported nonce during lookup;
they do not abbreviate or rewrite the control message.

Run:

```sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar \
  sh cache/formal/run_assignment_delivery_tlc.sh
```

The model is intentionally small. It does not duplicate scheduling, credit,
capacity, compiler input, or Protocol-50 cache state. Its only purpose is to
make the assignment-keyed formal actions realizable by the wire and to cover
the advisory claim/PREPARE race.
