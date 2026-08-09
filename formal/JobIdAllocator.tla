-------------------------- MODULE JobIdAllocator --------------------------
(***************************************************************************
A finite model of scheduler-visible job-ID allocation shared by remote Jobs
and local monitor Jobs.

IDs are 1..MaxId.  Zero is a sentinel and is never a valid live/terminal ID.
The model keeps a single allocator registry and explicit owner mappings so it
can expose the existing failure classes:

  * zero on wrap;
  * collision when only remote Jobs are checked;
  * map::operator[] turning unknown local Done into terminal ID zero;
  * duplicate local Begin overwriting one mapping and orphaning its old ID;
  * daemon disconnect removing the mapping but not the allocator ownership;
  * partial publication when a count-N request cannot reserve all remaining
    IDs.

Cumulative diagnostic counters saturate only to keep TLC's graph finite.  The
product counters remain wide monotonic telemetry.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Owners, RemoteOwners, LocalOwners, NoOwner,
          MaxId, InitialCursor, BatchCount, CountCap,
          OldOwner, NewOwner,
          MutantAllowZero,
          MutantCheckRemoteOnly,
          MutantUnknownDoneZero,
          MutantDuplicateOverwrite,
          MutantDisconnectLeak,
          MutantPartialBatch

Ids == 1..MaxId
OwnerPhases == {"Idle", "Live", "Terminal"}
BatchPhases == {"Idle", "Failed"}

ASSUME /\ IsFiniteSet(Owners)
       /\ Owners # {}
       /\ RemoteOwners \cup LocalOwners = Owners
       /\ RemoteOwners \cap LocalOwners = {}
       /\ NoOwner \notin Owners
       /\ MaxId \in Nat \ {0}
       /\ InitialCursor \in 0..MaxId
       /\ BatchCount \in 1..MaxId
       /\ CountCap \in Nat \ {0}
       /\ OldOwner \in Owners
       /\ NewOwner \in Owners
       /\ OldOwner # NewOwner
       /\ MutantAllowZero \in BOOLEAN
       /\ MutantCheckRemoteOnly \in BOOLEAN
       /\ MutantUnknownDoneZero \in BOOLEAN
       /\ MutantDuplicateOverwrite \in BOOLEAN
       /\ MutantDisconnectLeak \in BOOLEAN
       /\ MutantPartialBatch \in BOOLEAN

CapInc(n) == IF n < CountCap THEN n + 1 ELSE n

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

LiveOwners == {o \in Owners : ownerPhase[o] = "Live"}
LiveOwnerIds == {ownerId[o] : o \in LiveOwners}
ActualFree == Ids \ allocated

VisibleAllocated(o) ==
    IF MutantCheckRemoteOnly /\ o \in RemoteOwners
    THEN {ownerId[r] : r \in {x \in RemoteOwners : ownerPhase[x] = "Live"}}
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

UnknownLocalDone(o) ==
    /\ o \in LocalOwners
    /\ ownerPhase[o] # "Live"
    /\ unknownDoneCount' = CapInc(unknownDoneCount)
    /\ terminalZeroCount' =
          IF MutantUnknownDoneZero THEN CapInc(terminalZeroCount)
          ELSE terminalZeroCount
    /\ UNCHANGED <<ownerPhase, ownerId, allocated, cursor, issuedTotal,
                    releasedTotal, terminalCount, lastReleasedId,
                    reuseObserved, lastExhaustOwner, exhaustionCount,
                    unknownReleaseCount, duplicateBeginCount,
                    batchPhase, batchPublished>>

DuplicateLocalBegin(o) ==
    /\ o \in LocalOwners
    /\ ownerPhase[o] = "Live"
    /\ duplicateBeginCount' = CapInc(duplicateBeginCount)
    /\ LET overwrite == MutantDuplicateOverwrite /\ ActualFree # {}
           id == IF overwrite THEN FirstCyclic(ActualFree) ELSE ownerId[o]
       IN /\ ownerId' = IF overwrite THEN [ownerId EXCEPT ![o] = id] ELSE ownerId
          /\ allocated' = IF overwrite THEN allocated \cup {id} ELSE allocated
          /\ cursor' = IF overwrite THEN id ELSE cursor
          /\ issuedTotal' = IF overwrite THEN issuedTotal + 1 ELSE issuedTotal
    /\ UNCHANGED <<ownerPhase, releasedTotal, terminalCount, lastReleasedId,
                    reuseObserved, lastExhaustOwner, exhaustionCount,
                    unknownReleaseCount, unknownDoneCount, terminalZeroCount,
                    batchPhase, batchPublished>>

DisconnectLocal(o) ==
    /\ o \in LocalOwners
    /\ ownerPhase[o] = "Live"
    /\ terminalCount[o] = 0
    /\ LET id == ownerId[o]
       IN /\ ownerPhase' = [ownerPhase EXCEPT ![o] = "Terminal"]
          /\ allocated' =
                IF MutantDisconnectLeak THEN allocated ELSE allocated \ {id}
          /\ releasedTotal' =
                IF MutantDisconnectLeak THEN releasedTotal ELSE releasedTotal + 1
          /\ terminalCount' = [terminalCount EXCEPT ![o] = @ + 1]
          /\ lastReleasedId' =
                IF MutantDisconnectLeak THEN lastReleasedId ELSE id
    /\ UNCHANGED <<ownerId, cursor, issuedTotal, reuseObserved,
                    lastExhaustOwner, exhaustionCount, unknownReleaseCount,
                    unknownDoneCount, duplicateBeginCount, terminalZeroCount,
                    batchPhase, batchPublished>>

FailInsufficientBatch ==
    /\ batchPhase = "Idle"
    /\ Cardinality(ActualFree) < BatchCount
    /\ LET take ==
             IF MutantPartialBatch /\ ActualFree # {}
             THEN {FirstCyclic(ActualFree)}
             ELSE {}
       IN /\ batchPhase' = "Failed"
          /\ batchPublished' = take
          /\ allocated' = allocated \cup take
          /\ issuedTotal' = issuedTotal + Cardinality(take)
          /\ cursor' = IF take = {} THEN cursor ELSE CHOOSE id \in take : TRUE
    /\ UNCHANGED <<ownerPhase, ownerId, releasedTotal, terminalCount,
                    lastReleasedId, reuseObserved, lastExhaustOwner,
                    exhaustionCount, unknownReleaseCount, unknownDoneCount,
                    duplicateBeginCount, terminalZeroCount>>

Next ==
    \/ \E o \in Owners : Allocate(o)
    \/ \E o \in Owners : Release(o)
    \/ \E o \in Owners : UnknownRelease(o)
    \/ \E o \in Owners : Exhaust(o)
    \/ \E o \in LocalOwners : UnknownLocalDone(o)
    \/ \E o \in LocalOwners : DuplicateLocalBegin(o)
    \/ \E o \in LocalOwners : DisconnectLocal(o)
    \/ FailInsufficientBatch

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ ownerPhase \in [Owners -> OwnerPhases]
    /\ ownerId \in [Owners -> 0..MaxId]
    /\ allocated \in SUBSET (0..MaxId)
    /\ cursor \in 0..MaxId
    /\ issuedTotal \in Nat
    /\ releasedTotal \in Nat
    /\ terminalCount \in [Owners -> 0..1]
    /\ lastReleasedId \in 0..MaxId
    /\ reuseObserved \in BOOLEAN
    /\ lastExhaustOwner \in Owners \cup {NoOwner}
    /\ exhaustionCount \in 0..CountCap
    /\ unknownReleaseCount \in 0..CountCap
    /\ unknownDoneCount \in 0..CountCap
    /\ duplicateBeginCount \in 0..CountCap
    /\ terminalZeroCount \in 0..CountCap
    /\ batchPhase \in BatchPhases
    /\ batchPublished \in SUBSET Ids

NoZero ==
    /\ 0 \notin allocated
    /\ \A o \in LiveOwners : ownerId[o] # 0
    /\ terminalZeroCount = 0
    /\ issuedTotal > 0 => cursor # 0

GlobalLiveUniqueness ==
    \A a, b \in LiveOwners : a # b => ownerId[a] # ownerId[b]

AllocatorMapAgreement ==
    allocated = LiveOwnerIds

IssuedOnlyOnSuccess ==
    issuedTotal = Cardinality(allocated) + releasedTotal

ReleaseExactlyOnce ==
    \A o \in Owners : terminalCount[o] <= 1

ExhaustionNoPublication ==
    lastExhaustOwner = NoOwner
    \/ /\ ownerPhase[lastExhaustOwner] = "Idle"
       /\ ownerId[lastExhaustOwner] = 0

UnknownDoneNoTerminalZero == terminalZeroCount = 0

DuplicateBeginNoOrphan == allocated = LiveOwnerIds

ExhaustionNoPartialPublication ==
    batchPhase # "Failed" \/ batchPublished = {}

SafetyInvariant ==
    /\ TypeOK
    /\ NoZero
    /\ GlobalLiveUniqueness
    /\ AllocatorMapAgreement
    /\ IssuedOnlyOnSuccess
    /\ ReleaseExactlyOnce
    /\ ExhaustionNoPublication
    /\ UnknownDoneNoTerminalZero
    /\ DuplicateBeginNoOrphan
    /\ ExhaustionNoPartialPublication

(***************************************************************************
Controlled reuse scenario: one-ID domain, old owner initially live, new owner
waiting.  Exact release and fair allocation force reuse of ID 1 without ever
publishing zero or two simultaneous owners.
***************************************************************************)
ReuseInit ==
    /\ MaxId = 1
    /\ ownerPhase = [o \in Owners |-> IF o = OldOwner THEN "Live" ELSE "Idle"]
    /\ ownerId = [o \in Owners |-> IF o = OldOwner THEN 1 ELSE 0]
    /\ allocated = {1}
    /\ cursor = 1
    /\ issuedTotal = 1
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

ReuseNext == Release(OldOwner) \/ Allocate(NewOwner)

ReuseSpec ==
    /\ ReuseInit
    /\ [][ReuseNext]_vars
    /\ WF_vars(Release(OldOwner))
    /\ WF_vars(Allocate(NewOwner))

ReleasedIdEventuallyReusable == <> reuseObserved

=============================================================================
