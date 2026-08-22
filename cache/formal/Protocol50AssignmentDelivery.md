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
but it cannot change the phase from CLAIMED back to RESERVED.

A later REVOKE for that exact assignment returns:

```text
CLAIMED_OR_LATER
```

It does not install a revocation tombstone and it does not release scheduler
ownership. Only an exact `REVOKED` proof for an unclaimed assignment permits
release.

## Checked invariants

```text
PublishedHasMatchingPrepare
    UseCS publication is backed by READY for that exact assignment.

RevocationTargetsSentAssignment
    worker tombstone/revocation applies to the assignment named by S.

ReleaseHasMatchingWorkerProof
    scheduler release uses the exact assignment proved REVOKED by F.

ClaimedPhaseNeverRegresses
    late PREPARE or later REVOKE cannot move an accepted claim backward.

ClaimedAssignmentNeverReleased
    CLAIMED_OR_LATER is ownership evidence, not release proof.
```

## Mutants

The fail-closed runner requires these four unsafe variants to violate their
named invariants:

```text
READY resolved by current wire id rather than nonce
REVOKE request resolved by current wire id rather than nonce
REVOKE result resolved by current wire id rather than nonce
late advisory PREPARE regresses CLAIMED to RESERVED
```

Run:

```sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar \
  sh cache/formal/run_assignment_delivery_tlc.sh
```

The model is intentionally small. It does not duplicate scheduling, credit,
capacity, compiler input, or Protocol-50 cache state. Its only purpose is to
make the assignment-keyed formal actions realizable by the wire and to cover
the advisory claim/PREPARE/revoke race.
