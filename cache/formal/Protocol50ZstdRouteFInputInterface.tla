------------------------------ MODULE Protocol50ZstdRouteFInputInterface ------------------------------
(***************************************************************************
 Executable owner-side F-input boundary for Protocol-50 ZSTD_ROUTE.

 This module does not construct profile identities.  The core supplies one
 complete PreparedInput, one exact opaque CommitPermit, and one complete
 DurableBundle.  The owner retains those values unchanged, arbitrates cancel
 versus permit issuance, consumes each permit once, and keeps durable history
 after the current operation is closed or replaced.
***************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS NoOwner, NoPrepared, NoPermit, NoBundle

OwnerPhases == {"open", "closed"}
OwnerStages == {"receiving", "prepared", "permit-issued", "committed",
                 "cancelled"}
PreparedStages == {"prepared", "permit-issued", "committed", "cancelled"}
DeliveryStates == {"none", "open", "closed", "suppressed"}

PreparedFieldNames == {
    "route", "operation", "preparedId",
    "predecessorCursor", "predecessorDigest",
    "successorCursor", "successorTag", "successorDigest",
    "body", "raw", "contextGeneration", "codecJobId", "decoderTouched"
}

CompletePreparedInput(p) ==
    /\ p # NoPrepared
    /\ DOMAIN p = PreparedFieldNames
    /\ p.operation # NoOwner
    /\ p.preparedId # NoPrepared
    /\ p.contextGeneration \in Nat
    /\ p.contextGeneration > 0
    /\ p.codecJobId # NoPermit
    /\ p.decoderTouched \in BOOLEAN

InputRecordFieldNames == {"key", "operation", "preparedId", "raw", "body"}
ReadyFieldNames == {"readyEventId", "operation", "inputRecordKey"}
LastCommitFieldNames == {
    "operation", "preparedId", "permitId", "inputRecordKey", "ready",
    "predecessor", "successor", "raw", "body"
}
SuccessorFieldNames == {
    "route", "cursor", "tag", "digest", "contextGeneration", "codecJobId"
}
PredecessorFieldNames == {"route", "cursor", "digest"}
BundleFieldNames == {
    "operation", "preparedId", "permit", "inputRecord", "ready",
    "routeSuccessor", "lastCommit", "raw", "body", "predecessor",
    "successor", "contextGeneration", "codecJobId"
}

CompleteDurableBundle(b) ==
    /\ b # NoBundle
    /\ DOMAIN b = BundleFieldNames
    /\ b.operation # NoOwner
    /\ b.preparedId # NoPrepared
    /\ b.permit # NoPermit
    /\ DOMAIN b.inputRecord = InputRecordFieldNames
    /\ DOMAIN b.ready = ReadyFieldNames
    /\ DOMAIN b.lastCommit = LastCommitFieldNames
    /\ DOMAIN b.predecessor = PredecessorFieldNames
    /\ DOMAIN b.successor = SuccessorFieldNames
    /\ DOMAIN b.routeSuccessor = SuccessorFieldNames
    /\ b.ready.readyEventId \in Nat
    /\ b.ready.readyEventId > 0
    /\ b.contextGeneration \in Nat
    /\ b.contextGeneration > 0
    /\ b.inputRecord.key = b.ready.inputRecordKey
    /\ b.inputRecord.operation = b.operation
    /\ b.inputRecord.preparedId = b.preparedId
    /\ b.inputRecord.raw = b.raw
    /\ b.inputRecord.body = b.body
    /\ b.ready.operation = b.operation
    /\ b.lastCommit.operation = b.operation
    /\ b.lastCommit.preparedId = b.preparedId
    /\ b.lastCommit.permitId = b.permit
    /\ b.lastCommit.inputRecordKey = b.inputRecord.key
    /\ b.lastCommit.ready = b.ready
    /\ b.lastCommit.predecessor = b.predecessor
    /\ b.lastCommit.successor = b.successor
    /\ b.lastCommit.raw = b.raw
    /\ b.lastCommit.body = b.body
    /\ b.routeSuccessor = b.successor
    /\ b.successor.contextGeneration = b.contextGeneration
    /\ b.successor.codecJobId = b.codecJobId

BundleMatchesPrepared(b, p) ==
    /\ CompleteDurableBundle(b)
    /\ CompletePreparedInput(p)
    /\ b.operation = p.operation
    /\ b.preparedId = p.preparedId
    /\ b.raw = p.raw
    /\ b.body = p.body
    /\ b.predecessor.route = p.route
    /\ b.predecessor.cursor = p.predecessorCursor
    /\ b.predecessor.digest = p.predecessorDigest
    /\ b.successor.route = p.route
    /\ b.successor.cursor = p.successorCursor
    /\ b.successor.tag = p.successorTag
    /\ b.successor.digest = p.successorDigest
    /\ b.contextGeneration = p.contextGeneration
    /\ b.codecJobId = p.codecJobId

VARIABLE i

InitialState == [
    phase |-> "closed",
    stage |-> "receiving",
    owner |-> NoOwner,
    prepared |-> NoPrepared,
    permit |-> NoPermit,
    durable |-> NoBundle,
    lastDurable |-> NoBundle,
    durableBundles |-> {},
    issuedPermits |-> {},
    consumedPermits |-> {},
    readyEventIds |-> {},
    cancelledOperations |-> {},
    suppressedOperations |-> {},
    deliveredOperations |-> {},
    staleEventsRejected |-> 0,
    cancelRequested |-> FALSE,
    lateCloseSuppressed |-> FALSE,
    deliveryState |-> "none",
    lastObservation |-> NoOwner,
    lastEvent |-> "init"
]

Init == i = InitialState

DurableOperations(x) == {b.operation : b \in x.durableBundles}
DurablePermits(x) == {b.permit : b \in x.durableBundles}
DurableReadyIds(x) == {b.ready.readyEventId : b \in x.durableBundles}

(***************************************************************************
 ObservePrepared retains the exact complete core record.  A prior operation
 may already be durable; its bundle, permit, and Ready ledgers are persistent.
***************************************************************************)
ObservePrepared(preparedInput) ==
    /\ i.phase = "closed"
    /\ CompletePreparedInput(preparedInput)
    /\ preparedInput.operation \notin DurableOperations(i)
    /\ preparedInput.operation \notin i.cancelledOperations
    /\ i' = [i EXCEPT
        !.phase = "open",
        !.stage = "prepared",
        !.owner = preparedInput.operation,
        !.prepared = preparedInput,
        !.permit = NoPermit,
        !.durable = NoBundle,
        !.cancelRequested = FALSE,
        !.lateCloseSuppressed = FALSE,
        !.deliveryState = "open",
        !.lastObservation = NoOwner,
        !.lastEvent = "prepared-observed"]

Open(preparedInput) == ObservePrepared(preparedInput)

(***************************************************************************
 SelectCommit stores the exact opaque permit supplied by Core.  It does not
 mint or project one, and a historical permit cannot be selected again.
***************************************************************************)
SelectCommit(exactPermit) ==
    /\ i.phase = "open"
    /\ i.stage = "prepared"
    /\ i.permit = NoPermit
    /\ exactPermit # NoPermit
    /\ exactPermit \notin i.issuedPermits
    /\ exactPermit \notin i.consumedPermits
    /\ i' = [i EXCEPT
        !.stage = "permit-issued",
        !.permit = exactPermit,
        !.issuedPermits = @ \cup {exactPermit},
        !.lastEvent = "permit-issued"]

(***************************************************************************
 The single durable transition consumes the permit and installs the entire
 bundle, including one canonical nonzero Ready identity, in one owner state
 update.  All persistent ledgers advance in that same update.
***************************************************************************)
CommitDurable(exactBundle) ==
    /\ i.phase = "open"
    /\ i.stage = "permit-issued"
    /\ BundleMatchesPrepared(exactBundle, i.prepared)
    /\ exactBundle.operation = i.owner
    /\ exactBundle.permit = i.permit
    /\ i.permit \in i.issuedPermits
    /\ i.permit \notin i.consumedPermits
    /\ exactBundle.ready.readyEventId \notin i.readyEventIds
    /\ exactBundle.operation \notin DurableOperations(i)
    /\ i' = [i EXCEPT
        !.stage = "committed",
        !.permit = NoPermit,
        !.durable = exactBundle,
        !.lastDurable = exactBundle,
        !.durableBundles = @ \cup {exactBundle},
        !.consumedPermits = @ \cup {i.permit},
        !.readyEventIds = @ \cup {exactBundle.ready.readyEventId},
        !.lastEvent = "durable-commit"]

CancelObservation(op) == <<"owner-cancel-observation", op>>
CloseObservation(op) == <<"owner-close-observation", op>>

CancelWithObservation(observation) ==
    /\ i.phase = "open"
    /\ i.stage = "prepared"
    /\ i.permit = NoPermit
    /\ i.durable = NoBundle
    /\ observation # NoOwner
    /\ i.owner \notin DurableOperations(i)
    /\ i' = [i EXCEPT
        !.phase = "closed",
        !.stage = "cancelled",
        !.cancelRequested = TRUE,
        !.cancelledOperations = @ \cup {i.owner},
        !.deliveryState = "closed",
        !.lastObservation = observation,
        !.lastEvent = "cancel-before-permit"]

Cancel == CancelWithObservation(CancelObservation(i.owner))

SuppressDeliveryAfterCommit(observation) ==
    /\ i.phase = "open"
    /\ i.stage = "committed"
    /\ i.durable # NoBundle
    /\ observation # NoOwner
    /\ i' = [i EXCEPT
        !.phase = "closed",
        !.suppressedOperations = @ \cup {i.owner},
        !.lateCloseSuppressed = TRUE,
        !.deliveryState = "suppressed",
        !.lastObservation = observation,
        !.lastEvent = "late-close-suppressed"]

LateClose == SuppressDeliveryAfterCommit(CloseObservation(i.owner))

CloseCommitted ==
    /\ i.phase = "open"
    /\ i.stage = "committed"
    /\ i.durable # NoBundle
    /\ i' = [i EXCEPT
        !.phase = "closed",
        !.deliveredOperations = @ \cup {i.owner},
        !.deliveryState = "closed",
        !.lastObservation = CloseObservation(i.owner),
        !.lastEvent = "committed-delivery-closed"]

RetireCancelled ==
    /\ i.phase = "closed"
    /\ i.stage = "cancelled"
    /\ i' = [i EXCEPT
        !.stage = "receiving",
        !.owner = NoOwner,
        !.prepared = NoPrepared,
        !.cancelRequested = FALSE,
        !.deliveryState = "none",
        !.lastEvent = "cancelled-operation-retired"]

RejectStaleObservation(staleOperation, observation) ==
    /\ i.phase = "open"
    /\ staleOperation # i.owner
    /\ staleOperation # NoOwner
    /\ observation # NoOwner
    /\ i' = [i EXCEPT
        !.staleEventsRejected = @ + 1,
        !.lastObservation = observation,
        !.lastEvent = "stale-owner-event-rejected"]

PreparedRetained ==
    i.stage \in PreparedStages =>
        /\ CompletePreparedInput(i.prepared)
        /\ i.owner = i.prepared.operation

PermitLinear ==
    /\ (i.stage = "permit-issued" =>
        /\ i.permit # NoPermit
        /\ i.permit \in i.issuedPermits
        /\ i.permit \notin i.consumedPermits)
    /\ (i.stage # "permit-issued" => i.permit = NoPermit)
    /\ i.consumedPermits \subseteq i.issuedPermits
    /\ (i.stage = "permit-issued" =>
        i.issuedPermits = i.consumedPermits \cup {i.permit})
    /\ (i.stage # "permit-issued" =>
        i.issuedPermits = i.consumedPermits)
    /\ i.consumedPermits = DurablePermits(i)

DurableRetained ==
    /\ \A b \in i.durableBundles : CompleteDurableBundle(b)
    /\ i.readyEventIds = DurableReadyIds(i)
    /\ (i.lastDurable = NoBundle <=> i.durableBundles = {})
    /\ (i.lastDurable # NoBundle => i.lastDurable \in i.durableBundles)
    /\ (i.stage = "committed" =>
        /\ i.durable # NoBundle
        /\ i.durable \in i.durableBundles
        /\ i.durable.operation = i.owner)
    /\ (i.stage # "committed" => i.durable = NoBundle)

CancelBeforePermit ==
    /\ (i.stage = "cancelled" =>
        /\ i.phase = "closed"
        /\ i.owner \in i.cancelledOperations
        /\ i.owner \notin DurableOperations(i)
        /\ i.permit = NoPermit
        /\ i.durable = NoBundle)
    /\ i.cancelledOperations \cap DurableOperations(i) = {}

LateClosePreservesDurable ==
    i.lateCloseSuppressed =>
        /\ i.phase = "closed"
        /\ i.stage = "committed"
        /\ i.deliveryState = "suppressed"
        /\ i.owner \in i.suppressedOperations
        /\ i.durable \in i.durableBundles
        /\ i.durable = i.lastDurable

NoPermitReuse ==
    /\ Cardinality(i.consumedPermits) = Cardinality(i.durableBundles)
    /\ Cardinality(i.readyEventIds) = Cardinality(i.durableBundles)

OwnerTypeOK ==
    /\ i.phase \in OwnerPhases
    /\ i.stage \in OwnerStages
    /\ i.cancelRequested \in BOOLEAN
    /\ i.lateCloseSuppressed \in BOOLEAN
    /\ i.deliveryState \in DeliveryStates
    /\ i.staleEventsRejected \in Nat
    /\ IsFiniteSet(i.durableBundles)
    /\ IsFiniteSet(i.issuedPermits)
    /\ IsFiniteSet(i.consumedPermits)
    /\ IsFiniteSet(i.readyEventIds)
    /\ \A id \in i.readyEventIds : id \in Nat /\ id > 0
    /\ IsFiniteSet(i.cancelledOperations)
    /\ IsFiniteSet(i.suppressedOperations)
    /\ IsFiniteSet(i.deliveredOperations)
    /\ PreparedRetained
    /\ PermitLinear
    /\ DurableRetained
    /\ CancelBeforePermit
    /\ LateClosePreservesDurable
    /\ NoPermitReuse

=============================================================================
