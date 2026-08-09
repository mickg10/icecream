--------------------------- MODULE JobIdAllocator ---------------------------
(***************************************************************************
Finite model of the scheduler's shared nonzero job-id allocator.

The allocator owns one namespace used by remote jobs and local-monitor jobs.
An id is reserved before publication and released exactly once at the terminal
transition.  Allocation exhausts explicitly instead of spinning or publishing
a partial batch.

Counter switches remove one load-bearing rule each:

  MutantRemoteOnlyScan       ignores local-monitor allocations;
  MutantAllowZero            permits the reserved wire id zero;
  MutantPartialBatch         publishes a proper prefix when the batch cannot
                            reserve every requested id;
  MutantDuplicateBeginsNew   treats a duplicate local begin as a fresh begin.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Owners, RemoteOwners, LocalOwners, NoOwner,
          MaxId, InitialCursor, BatchOwners, CountCap,
          MutantRemoteOnlyScan, MutantAllowZero,
          MutantPartialBatch, MutantDuplicateBeginsNew

ASSUME /\ IsFiniteSet(Owners)
       /\ Owners # {}
       /\ IsFiniteSet(RemoteOwners)
       /\ IsFiniteSet(LocalOwners)
       /\ RemoteOwners \subseteq Owners
       /\ LocalOwners \subseteq Owners
       /\ RemoteOwners \cap LocalOwners = {}
       /\ RemoteOwners \cup LocalOwners = Owners
       /\ NoOwner \notin Owners
       /\ MaxId \in Nat \ {0}
       /\ InitialCursor \in 0..MaxId
       /\ IsFiniteSet(BatchOwners)
       /\ BatchOwners \subseteq Owners
       /\ BatchOwners # {}
       /\ CountCap \in Nat \ {0}
       /\ MutantRemoteOnlyScan \in BOOLEAN
       /\ MutantAllowZero \in BOOLEAN
       /\ MutantPartialBatch \in BOOLEAN
       /\ MutantDuplicateBeginsNew \in BOOLEAN

Ids == 1..MaxId
OwnerPhases == {"Idle", "Live", "Terminal"}
BatchPhases == {"Idle", "Published", "Failed"}

VARIABLES ownerPhase,
          ownerId,
          allocated,
          cursor,
          issuedTotal,
          releasedTotal,
          terminalCount,
          lastReleasedId,
          reuseObserved,
          lastExhaustOwner,
          exhaustionCount,
          unknownReleaseCount,
          unknownDoneCount,
          duplicateBeginCount,
          terminalZeroCount,
          batchPhase,
          batchPublished

vars ==
    <<ownerPhase, ownerId, allocated, cursor, issuedTotal, releasedTotal,
      terminalCount, lastReleasedId, reuseObserved, lastExhaustOwner,
      exhaustionCount, unknownReleaseCount, unknownDoneCount,
      duplicateBeginCount, terminalZeroCount, batchPhase, batchPublished>>

CapInc(n) == IF n < CountCap THEN n + 1 ELSE n

VisibleAllocated(o) ==
    IF MutantRemoteOnlyScan /\ o \in RemoteOwners
    THEN {ownerId[x] : x \in RemoteOwners /\ ownerPhase[x] = "Live"}
    ELSE allocated

CandidateFree(o) == Ids \ VisibleAllocated(o)

DistanceFromCursor(id) ==
    IF id > cursor THEN id - cursor ELSE MaxId - cursor + id

FirstCyclic(s) ==
    CHOOSE id \in s :
        \A other \in s : DistanceFromCursor(id) <= DistanceFromCursor(other)

SelectedId(o) ==
    IF MutantAllowZero /\ cursor = MaxId
    THEN 0
    ELSE FirstCyclic(CandidateFree(o))

Init ==
    /\ ownerPhase = [o \in Owners |-> "Idle"]
    /\ ownerId = [o \in Owners |-> 0]
    /\ allocated = {}
    /\ cursor = InitialCursor
    /\ issuedTotal = 0
    /\ releasedTotal = 0
    /\ terminalCount = [o \in Owners |-> 0]
    /\ lastReleasedId = 0
    /\ reuseObserved = FALSE
    /\ lastExhaustOwner = NoOwner
    /\ exhaustionCount = 0
    /\ unknownReleaseCount = 0
    /\ unknownDoneCount = 0
    /\ duplicateBeginCount = 0
    /\ terminalZeroCount = 0
    /\ batchPhase = "Idle"
    /\ batchPublished = {}

Allocate(o) ==
    /\ o \in Owners
    /\ ownerPhase[o] = "Idle"
    /\ CandidateFree(o) # {}
    /\ LET id == SelectedId(o)
       IN /\ ownerPhase' = [ownerPhase EXCEPT ![o] = "Live"]
          /\ ownerId' = [ownerId EXCEPT ![o] = id]
          /\ allocated' = allocated \cup {id}
          /\ cursor' = id
          /\ issuedTotal' = issuedTotal + 1
          /\ reuseObserved' =
                (reuseObserved
                 \/ (lastReleasedId # 0 /\ id = lastReleasedId))
    /\ lastExhaustOwner' = NoOwner
    /\ UNCHANGED <<releasedTotal, terminalCount, lastReleasedId,
                    exhaustionCount, unknownReleaseCount, unknownDoneCount,
                    duplicateBeginCount, terminalZeroCount,
                    batchPhase, batchPublished>>

Release(o) ==
    /\ o \in Owners
    /\ ownerPhase[o] = "Live"
    /\ terminalCount[o] = 0
    /\ LET id == ownerId[o]
       IN /\ ownerPhase' = [ownerPhase EXCEPT ![o] = "Terminal"]
          /\ allocated' = allocated \ {id}
          /\ releasedTotal' = releasedTotal + 1
          /\ terminalCount' = [terminalCount EXCEPT ![o] = @ + 1]
          /\ lastReleasedId' = id
    /\ UNCHANGED <<ownerId, cursor, issuedTotal, reuseObserved,
                    lastExhaustOwner, exhaustionCount, unknownReleaseCount,
                    unknownDoneCount, duplicateBeginCount, terminalZeroCount,
                    batchPhase, batchPublished>>

UnknownRelease(o) ==
    /\ o \in Owners
    /\ ownerPhase[o] # "Live"
    /\ unknownReleaseCount' = CapInc(unknownReleaseCount)
    /\ UNCHANGED <<ownerPhase, ownerId, allocated, cursor, issuedTotal,
                    releasedTotal, terminalCount, lastReleasedId,
                    reuseObserved, lastExhaustOwner, exhaustionCount,
                    unknownDoneCount, duplicateBeginCount, terminalZeroCount,
                    batchPhase, batchPublished>>

Exhaust(o) ==
    /\ o \in Owners
    /\ ownerPhase[o] = "Idle"
    /\ CandidateFree(o) = {}
    /\ lastExhaustOwner' = o
    /\ exhaustionCount' = CapInc(exhaustionCount)
    /\ UNCHANGED <<ownerPhase, ownerId, allocated, cursor, issuedTotal,
                    releasedTotal, terminalCount, lastReleasedId,
                    reuseObserved, unknownReleaseCount, unknownDoneCount,
                    duplicateBeginCount, terminalZeroCount,
                    batchPhase, batchPublished>>

UnknownDone(o) ==
    /\ o \in Owners
    /\ ownerPhase[o] # "Live"
    /\ unknownDoneCount' = CapInc(unknownDoneCount)
    /\ UNCHANGED <<ownerPhase, ownerId, allocated, cursor, issuedTotal,
                    releasedTotal, terminalCount, lastReleasedId,
                    reuseObserved, lastExhaustOwner, exhaustionCount,
                    unknownReleaseCount, duplicateBeginCount,
                    terminalZeroCount, batchPhase, batchPublished>>

DuplicateLocalBegin(o) ==
    /\ o \in LocalOwners
    /\ ownerPhase[o] = "Live"
    /\ duplicateBeginCount' = CapInc(duplicateBeginCount)
    /\ IF MutantDuplicateBeginsNew
          THEN /\ CandidateFree(o) # {}
               /\ LET id == SelectedId(o)
                  IN /\ ownerId' = [ownerId EXCEPT ![o] = id]
                     /\ allocated' = (allocated \ {ownerId[o]}) \cup {id}
                     /\ cursor' = id
                     /\ issuedTotal' = issuedTotal + 1
          ELSE UNCHANGED <<ownerId, allocated, cursor, issuedTotal>>
    /\ UNCHANGED <<ownerPhase, releasedTotal, terminalCount,
                    lastReleasedId, reuseObserved, lastExhaustOwner,
                    exhaustionCount, unknownReleaseCount, unknownDoneCount,
                    terminalZeroCount, batchPhase, batchPublished>>

RecordTerminalZero ==
    /\ 0 \in allocated
    /\ terminalZeroCount' = CapInc(terminalZeroCount)
    /\ UNCHANGED <<ownerPhase, ownerId, allocated, cursor, issuedTotal,
                    releasedTotal, terminalCount, lastReleasedId,
                    reuseObserved, lastExhaustOwner, exhaustionCount,
                    unknownReleaseCount, unknownDoneCount,
                    duplicateBeginCount, batchPhase, batchPublished>>

StartBatch ==
    /\ batchPhase = "Idle"
    /\ \A o \in BatchOwners : ownerPhase[o] = "Idle"
    /\ LET free == Ids \ allocated
       IN /\ IF Cardinality(free) >= Cardinality(BatchOwners)
                 THEN /\ batchPhase' = "Published"
                      /\ batchPublished' = BatchOwners
                      /\ ownerPhase' =
                            [ownerPhase EXCEPT
                               ![o \in BatchOwners] = "Live"]
                      /\ ownerId' =
                            [ownerId EXCEPT
                               ![o \in BatchOwners] =
                                  CHOOSE id \in free : TRUE]
                      /\ allocated' =
                            allocated \cup
                            {ownerId'[o] : o \in BatchOwners}
                      /\ issuedTotal' =
                            issuedTotal + Cardinality(BatchOwners)
                 ELSE IF MutantPartialBatch /\ free # {}
                         THEN /\ LET chosenOwner == CHOOSE o \in BatchOwners : TRUE
                                      chosenId == CHOOSE id \in free : TRUE
                                  IN /\ batchPhase' = "Failed"
                                     /\ batchPublished' = {chosenOwner}
                                     /\ ownerPhase' =
                                           [ownerPhase EXCEPT
                                              ![chosenOwner] = "Live"]
                                     /\ ownerId' =
                                           [ownerId EXCEPT
                                              ![chosenOwner] = chosenId]
                                     /\ allocated' = allocated \cup {chosenId}
                                     /\ issuedTotal' = issuedTotal + 1
                         ELSE /\ batchPhase' = "Failed"
                              /\ batchPublished' = {}
                              /\ UNCHANGED <<ownerPhase, ownerId, allocated,
                                              issuedTotal>>
    /\ UNCHANGED <<cursor, releasedTotal, terminalCount,
                    lastReleasedId, reuseObserved, lastExhaustOwner,
                    exhaustionCount, unknownReleaseCount, unknownDoneCount,
                    duplicateBeginCount, terminalZeroCount>>

Next ==
    \/ \E o \in Owners : Allocate(o)
    \/ \E o \in Owners : Release(o)
    \/ \E o \in Owners : UnknownRelease(o)
    \/ \E o \in Owners : Exhaust(o)
    \/ \E o \in Owners : UnknownDone(o)
    \/ \E o \in LocalOwners : DuplicateLocalBegin(o)
    \/ RecordTerminalZero
    \/ StartBatch

TypeOK ==
    /\ ownerPhase \in [Owners -> OwnerPhases]
    /\ ownerId \in [Owners -> 0..MaxId]
    /\ allocated \subseteq 0..MaxId
    /\ cursor \in 0..MaxId
    /\ issuedTotal \in Nat
    /\ releasedTotal \in Nat
    /\ terminalCount \in [Owners -> Nat]
    /\ lastReleasedId \in 0..MaxId
    /\ reuseObserved \in BOOLEAN
    /\ lastExhaustOwner \in Owners \cup {NoOwner}
    /\ exhaustionCount \in Nat
    /\ unknownReleaseCount \in Nat
    /\ unknownDoneCount \in Nat
    /\ duplicateBeginCount \in Nat
    /\ terminalZeroCount \in Nat
    /\ batchPhase \in BatchPhases
    /\ batchPublished \subseteq BatchOwners

NoZeroAllocated == 0 \notin allocated
LiveIdsExactlyAllocated ==
    allocated = {ownerId[o] : o \in Owners /\ ownerPhase[o] = "Live"}
LiveIdsUnique ==
    \A o1, o2 \in Owners :
        o1 # o2 /\ ownerPhase[o1] = "Live" /\ ownerPhase[o2] = "Live"
        => ownerId[o1] # ownerId[o2]
TerminalAtMostOnce ==
    \A o \in Owners : terminalCount[o] <= 1
ReleaseConservation == releasedTotal <= issuedTotal
NoPartialBatchPublication ==
    batchPhase = "Failed" => batchPublished = {}
DuplicateBeginIdempotent ==
    duplicateBeginCount > 0 => issuedTotal = Cardinality(allocated) + releasedTotal
UnknownReleaseRecorded == unknownReleaseCount <= CountCap
UnknownDoneRecorded == unknownDoneCount <= CountCap
TerminalZeroNeverEmitted == terminalZeroCount = 0

AllocatorInvariant ==
    /\ TypeOK
    /\ NoZeroAllocated
    /\ LiveIdsExactlyAllocated
    /\ LiveIdsUnique
    /\ TerminalAtMostOnce
    /\ ReleaseConservation
    /\ NoPartialBatchPublication
    /\ DuplicateBeginIdempotent
    /\ UnknownReleaseRecorded
    /\ UnknownDoneRecorded
    /\ TerminalZeroNeverEmitted

Spec == Init /\ [][Next]_vars

ReuseFairSpec ==
    /\ Spec
    /\ WF_vars(\E o \in Owners : Allocate(o))
    /\ WF_vars(\E o \in Owners : Release(o))

ReleasedIdEventuallyReusable == <>reuseObserved

=============================================================================
