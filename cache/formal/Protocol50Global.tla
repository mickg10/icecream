-------------------------- MODULE Protocol50Global --------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Bounded S3 global resource model.

This model is deliberately separate from Protocol50.tla.  Protocol50.tla
owns one transaction on one C/F relationship; this module owns the shared
resource boundary across two relationships.  In particular, the staging
slot map and byte accounting are global, while the arena and LRU metadata
are keyed by C namespace.

The only legal object lifecycle is:

    ABSENT -> INSTALLING -> PRESENT -> PINNED

INSTALLING is owned by exactly one global staging slot.  A crash removes the
partial object and releases its slot.  A retry is therefore idempotent: it
starts from ABSENT and can publish the canonical content at most once.

The model is safety-scoped.  It includes a bounded deterministic writer
watchdog rather than an unbounded liveness claim: its counter advances only
when an INSTALLING object still owns a slot and the explicit stall mutant has
blocked that enabled writer.  Reaching the finite deadline is a fail-closed
state that the watchdog invariant rejects.
***************************************************************************)

CONSTANTS N0, N1, K0, K1, S0, S1,
          G0, G1, G2, NoGuid, NoKey, NoNamespace, NoOwner,
          NoContent, V0, V1,
          MaxGeneration, MaxAggregateBytes, MaxNamespaceBytes,
          MaxStagingBytes, MaxTotalBytes,
          MutantIgnoreAggregateCap,
          MutantIgnoreNamespaceCap,
          MutantIgnoreStagingCap,
          MutantIgnoreTotalCap,
          MutantIgnoreSlotOwnership,
          MutantIgnoreLru,
          MutantLocalLru,
          MutantWrapGeneration,
          MutantAdmitWhileStopped,
          MutantReuseGuid,
          MutantCrossNamespaceAlias,
          MutantKeepLifecycleOnEvict,
          MutantIgnoreConflict,
          MutantCrashKeepsSlot,
          MutantBadContent,
          MutantStallWriter

ASSUME /\ N0 # N1
       /\ K0 # K1
       /\ S0 # S1
       /\ G0 # G1
       /\ G1 # G2
       /\ G0 # G2
       /\ NoGuid # G0
       /\ NoGuid # G1
       /\ NoGuid # G2
       /\ NoKey # K0
       /\ NoKey # K1
       /\ NoNamespace # N0
       /\ NoNamespace # N1
       /\ MaxGeneration \in Nat
       /\ MaxGeneration > 0
       /\ MaxAggregateBytes \in Nat
       /\ MaxNamespaceBytes \in Nat
       /\ MaxAggregateBytes >= MaxNamespaceBytes
       /\ MaxStagingBytes \in Nat
       /\ MaxTotalBytes \in Nat
       /\ MaxTotalBytes >= MaxAggregateBytes
       /\ MutantIgnoreAggregateCap \in BOOLEAN
       /\ MutantIgnoreNamespaceCap \in BOOLEAN
       /\ MutantIgnoreStagingCap \in BOOLEAN
       /\ MutantIgnoreTotalCap \in BOOLEAN
       /\ MutantIgnoreSlotOwnership \in BOOLEAN
       /\ MutantIgnoreLru \in BOOLEAN
       /\ MutantLocalLru \in BOOLEAN
       /\ MutantWrapGeneration \in BOOLEAN
       /\ MutantAdmitWhileStopped \in BOOLEAN
       /\ MutantReuseGuid \in BOOLEAN
       /\ MutantCrossNamespaceAlias \in BOOLEAN
       /\ MutantKeepLifecycleOnEvict \in BOOLEAN
       /\ MutantIgnoreConflict \in BOOLEAN
       /\ MutantCrashKeepsSlot \in BOOLEAN
       /\ MutantBadContent \in BOOLEAN
       /\ MutantStallWriter \in BOOLEAN

Namespaces == {N0, N1}
Keys == {K0, K1}
Slots == {S0, S1}
Guids == {G0, G1, G2}
Generations == 0..MaxGeneration
Values == {V0, V1}
ObjectStates == {"ABSENT", "INSTALLING", "PRESENT", "PINNED"}
NoOwnerValue == <<NoNamespace, NoKey>>

CanonicalValue(n, k) ==
    IF k = K0
    THEN IF n = N0 THEN V0 ELSE V1
    ELSE IF n = N0 THEN V1 ELSE V0
ObjectBytes(k) == IF k = K0 THEN 2 ELSE 3
Owner(n, k) == <<n, k>>

VARIABLES s, step
vars == <<s, step>>

Init ==
    /\ s = [live                 |-> [n \in Namespaces |-> FALSE],
         evicted              |-> [n \in Namespaces |-> FALSE],
         admissionCount       |-> [n \in Namespaces |-> 0],
         guid                 |-> [n \in Namespaces |-> IF n = N0 THEN G0 ELSE G2],
         guidHistory          |-> [n \in Namespaces |->
                                      {IF n = N0 THEN G0 ELSE G2}],
         generation           |-> [n \in Namespaces |-> 0],
         generationSeen       |-> [n \in Namespaces |-> {0}],
         admissionStopped     |-> [n \in Namespaces |-> FALSE],
         lru                  |-> [n \in Namespaces |-> 0],
         clock                |-> 0,
         touchCount           |-> [n \in Namespaces |-> 0],
         active               |-> [n \in Namespaces |-> FALSE],
         tuUsed               |-> [n \in Namespaces |-> FALSE],
         freshAdmissionPending |-> [n \in Namespaces |-> FALSE],
         arena                |-> [n \in Namespaces |->
                                      [k \in Keys |-> "ABSENT"]],
         content              |-> [n \in Namespaces |->
                                      [k \in Keys |-> NoContent]],
         stagingContent       |-> [n \in Namespaces |->
                                      [k \in Keys |-> NoContent]],
         arenaGuid            |-> [n \in Namespaces |->
                                      [k \in Keys |-> NoGuid]],
         arenaGeneration      |-> [n \in Namespaces |->
                                      [k \in Keys |-> 0]],
         installAttempts      |-> [n \in Namespaces |->
                                      [k \in Keys |-> 0]],
         pinUsed              |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         slotOwner            |-> [slot \in Slots |-> NoOwnerValue],
         crashed              |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         retrySeen            |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         conflictSeen         |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         writerBlocked        |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         watchdogCount       |-> [n \in Namespaces |->
                                      [k \in Keys |-> 0]],
         watchdogExpired     |-> FALSE,
         fatal                |-> FALSE,
         badAggregate         |-> FALSE,
         badNamespaceCap      |-> FALSE,
         badSlot               |-> FALSE,
         badLru                |-> FALSE,
         badGeneration         |-> FALSE,
         badAdmission          |-> FALSE,
         badGuidReuse          |-> FALSE,
         badConflict           |-> FALSE,
         badCrash              |-> FALSE,
         badContent            |-> FALSE,
         badCrossNamespaceGuid |-> FALSE]
    /\ step = 0

WatchdogLimit == 2
MaxSteps == 14

WriterWorkEnabled(st, n, k) ==
    /\ st.arena[n][k] = "INSTALLING"
    /\ \E slot \in Slots : st.slotOwner[slot] = Owner(n, k)

ByteCharge(st, n, k) ==
    IF st.arena[n][k] \in {"PRESENT", "PINNED"}
    THEN ObjectBytes(k)
    ELSE 0

NsBytes(st, n) ==
    ByteCharge(st, n, K0) + ByteCharge(st, n, K1)

TotalBytes(st) == NsBytes(st, N0) + NsBytes(st, N1)

StagingBytes(st, n, k) ==
    IF st.arena[n][k] = "INSTALLING" THEN ObjectBytes(k) ELSE 0

NsStagingBytes(st, n) ==
    StagingBytes(st, n, K0) + StagingBytes(st, n, K1)

TotalStagingBytes(st) == NsStagingBytes(st, N0) + NsStagingBytes(st, N1)

TotalSimultaneousBytes(st) == TotalBytes(st) + TotalStagingBytes(st)

Installing(st, n) ==
    {k \in Keys : st.arena[n][k] = "INSTALLING"}

Pinned(st, n) ==
    {k \in Keys : st.arena[n][k] = "PINNED"}

NamespaceEligible(st, n) ==
    /\ st.live[n]
    /\ ~st.active[n]
    /\ Installing(st, n) = {}
    /\ Pinned(st, n) = {}

OldestEligible(st, n) ==
    /\ n \in Namespaces
    /\ NamespaceEligible(st, n)
    /\ \A other \in Namespaces :
           NamespaceEligible(st, other) => st.lru[n] <= st.lru[other]

ClearNamespace(st, n) ==
    [st EXCEPT
        !.live[n] = FALSE,
        !.evicted[n] = TRUE,
        !.active[n] = FALSE,
        !.tuUsed[n] = IF MutantKeepLifecycleOnEvict THEN st.tuUsed[n] ELSE FALSE,
        !.freshAdmissionPending[n] = FALSE,
        !.arena[n] = [k \in Keys |-> "ABSENT"],
        !.content[n] = [k \in Keys |-> NoContent],
        !.stagingContent[n] = [k \in Keys |-> NoContent],
        !.arenaGuid[n] = [k \in Keys |-> NoGuid],
        !.arenaGeneration[n] = [k \in Keys |-> st.generation[n]],
        !.installAttempts[n] = [k \in Keys |->
                                  IF MutantKeepLifecycleOnEvict
                                  THEN st.installAttempts[n][k] ELSE 0],
        !.pinUsed[n] = [k \in Keys |->
                          IF MutantKeepLifecycleOnEvict
                          THEN st.pinUsed[n][k] ELSE FALSE],
        !.crashed[n] = [k \in Keys |->
                          IF MutantKeepLifecycleOnEvict
                          THEN st.crashed[n][k] ELSE FALSE],
        !.retrySeen[n] = [k \in Keys |->
                            IF MutantKeepLifecycleOnEvict
                            THEN st.retrySeen[n][k] ELSE FALSE],
        !.conflictSeen[n] = [k \in Keys |->
                               IF MutantKeepLifecycleOnEvict
                               THEN st.conflictSeen[n][k] ELSE FALSE],
        !.writerBlocked[n] = [k \in Keys |-> FALSE],
        !.watchdogCount[n] = [k \in Keys |-> 0],
        !.slotOwner = [slot \in Slots |->
                          IF st.slotOwner[slot][1] = n
                          THEN NoOwnerValue ELSE st.slotOwner[slot]]]

ADMIT_NAMESPACE(n) ==
    LET rejected == s.admissionStopped[n]
    IN /\ n \in Namespaces
       /\ ~s.live[n]
       /\ s.admissionCount[n] < 2
       /\ (s.generation[n] < MaxGeneration \/ s.admissionStopped[n] \/
             MutantAdmitWhileStopped)
       /\ (~s.admissionStopped[n] \/ MutantAdmitWhileStopped)
       /\ s' = [s EXCEPT
                    !.live[n] = TRUE,
                    !.evicted[n] = FALSE,
                    !.admissionCount[n] = @ + 1,
                    !.freshAdmissionPending = [m \in Namespaces |->
                        IF m = n THEN s.admissionCount[n] = 1
                        ELSE s.freshAdmissionPending[m]],
                    !.badAdmission = @ \/ rejected \/
                        (s.generation[n] = MaxGeneration /\
                         ~s.admissionStopped[n])]

TOUCH_NAMESPACE(n) ==
    /\ n \in Namespaces
    /\ s.live[n]
    /\ s' = [s EXCEPT
                 !.clock = @ + 1,
                 !.lru[n] = IF MutantLocalLru THEN s.lru[n] + 1 ELSE s.clock + 1,
                 !.touchCount[n] = @ + 1]

START_TU(n) ==
    /\ n \in Namespaces
    /\ s.live[n]
    /\ ~s.active[n]
    /\ ~s.tuUsed[n]
    /\ s' = [s EXCEPT
                 !.active[n] = TRUE,
                 !.tuUsed[n] = TRUE,
                 !.freshAdmissionPending[n] = FALSE]

FINISH_TU(n) ==
    /\ n \in Namespaces
    /\ s.active[n]
    /\ Installing(s, n) = {}
    /\ s' = [s EXCEPT
                 !.active[n] = FALSE,
                 !.arena[n] = [k \in Keys |->
                     IF s.arena[n][k] = "PINNED" THEN "PRESENT"
                     ELSE s.arena[n][k]]]

BEGIN_INSTALL(n, k, slot, value) ==
    LET slotBusy == s.slotOwner[slot] # NoOwnerValue
        wasRetry == s.crashed[n][k]
    IN /\ n \in Namespaces
       /\ k \in Keys
       /\ slot \in Slots
       /\ value \in Values
       /\ s.live[n]
       /\ s.active[n]
       /\ s.arena[n][k] = "ABSENT"
       /\ ~s.crashed[n][k]
       /\ s.installAttempts[n][k] = 0
       /\ (s.slotOwner[slot] = NoOwnerValue \/ MutantIgnoreSlotOwnership)
       /\ (TotalStagingBytes(s) + ObjectBytes(k) <= MaxStagingBytes \/
             MutantIgnoreStagingCap)
       /\ (TotalSimultaneousBytes(s) + ObjectBytes(k) <= MaxTotalBytes \/
             MutantIgnoreTotalCap)
       /\ s' = [s EXCEPT
                    !.arena[n][k] = "INSTALLING",
                    !.stagingContent[n][k] = value,
                    !.arenaGuid[n][k] = s.guid[n],
                    !.arenaGeneration[n][k] = s.generation[n],
                    !.installAttempts[n][k] = 1,
                    !.slotOwner[slot] = Owner(n, k),
                    !.retrySeen[n][k] = @ \/ wasRetry,
                    !.badSlot = @ \/ slotBusy]

RETRY_INSTALL(n, k, slot, value) ==
    /\ n \in Namespaces
    /\ k \in Keys
    /\ slot \in Slots
    /\ value \in Values
    /\ s.crashed[n][k]
    /\ s.arena[n][k] = "ABSENT"
    /\ s.live[n]
    /\ s.active[n]
    /\ s.installAttempts[n][k] = 1
    /\ s.slotOwner[slot] = NoOwnerValue
    /\ (TotalStagingBytes(s) + ObjectBytes(k) <= MaxStagingBytes \/
          MutantIgnoreStagingCap)
    /\ (TotalSimultaneousBytes(s) + ObjectBytes(k) <= MaxTotalBytes \/
          MutantIgnoreTotalCap)
    /\ s' = [s EXCEPT
                 !.arena[n][k] = "INSTALLING",
                 !.stagingContent[n][k] = value,
                 !.arenaGuid[n][k] = s.guid[n],
                 !.arenaGeneration[n][k] = s.generation[n],
                 !.installAttempts[n][k] = 2,
                 !.slotOwner[slot] = Owner(n, k),
                 !.retrySeen[n][k] = TRUE]

PUBLISH_INSTALL(n, k, slot) ==
    LET value == s.stagingContent[n][k]
        nextNamespaceBytes == NsBytes(s, n) + ObjectBytes(k)
        nextTotalBytes == TotalBytes(s) + ObjectBytes(k)
        canonical == value = CanonicalValue(n, k)
    IN /\ n \in Namespaces
       /\ k \in Keys
       /\ slot \in Slots
       /\ s.live[n]
       /\ s.arena[n][k] = "INSTALLING"
       /\ s.slotOwner[slot] = Owner(n, k)
       /\ value \in Values
       /\ canonical \/ MutantBadContent
       /\ (nextNamespaceBytes <= MaxNamespaceBytes \/
             MutantIgnoreNamespaceCap)
       /\ (nextTotalBytes <= MaxAggregateBytes \/ MutantIgnoreAggregateCap)
       /\ s' = [s EXCEPT
                    !.arena[n][k] = "PRESENT",
                    !.content[n][k] =
                        IF MutantBadContent THEN value ELSE CanonicalValue(n, k),
                    !.stagingContent[n][k] = NoContent,
                    !.writerBlocked[n][k] = FALSE,
                    !.watchdogCount[n][k] = 0,
                    !.slotOwner[slot] = NoOwnerValue,
                    !.badContent = @ \/ ~canonical,
                    !.badNamespaceCap = @ \/
                        nextNamespaceBytes > MaxNamespaceBytes,
                    !.badAggregate = @ \/
                        nextTotalBytes > MaxAggregateBytes]

PIN_OBJECT(n, k) ==
    /\ n \in Namespaces
    /\ k \in Keys
    /\ s.active[n]
    /\ s.arena[n][k] = "PRESENT"
    /\ ~s.pinUsed[n][k]
    /\ s' = [s EXCEPT
                 !.arena[n][k] = "PINNED",
                 !.pinUsed[n][k] = TRUE]

UNPIN_OBJECT(n, k) ==
    /\ n \in Namespaces
    /\ k \in Keys
    /\ s.arena[n][k] = "PINNED"
    /\ s' = [s EXCEPT !.arena[n][k] = "PRESENT"]

CRASH_MID_INSTALL(n, k, slot) ==
    /\ n \in Namespaces
    /\ k \in Keys
    /\ slot \in Slots
    /\ s.arena[n][k] = "INSTALLING"
    /\ s.slotOwner[slot] = Owner(n, k)
    /\ s' = [s EXCEPT
                 !.arena[n][k] = "ABSENT",
                 !.stagingContent[n][k] = NoContent,
                 !.arenaGuid[n][k] = NoGuid,
                 !.arenaGeneration[n][k] = s.generation[n],
                 !.writerBlocked[n][k] = FALSE,
                 !.watchdogCount[n][k] = 0,
                 !.slotOwner[slot] =
                     IF MutantCrashKeepsSlot THEN @ ELSE NoOwnerValue,
                 !.crashed[n][k] = TRUE,
                 !.badCrash = @ \/ MutantCrashKeepsSlot]

CONFLICTING_CONTENT(n, k, value) ==
    /\ n \in Namespaces
    /\ k \in Keys
    /\ value \in Values
    /\ ~s.conflictSeen[n][k]
    /\ s.content[n][k] \in Values
    /\ value # s.content[n][k]
    /\ s.arena[n][k] \in {"PRESENT", "PINNED"}
    /\ s' = [s EXCEPT
                 !.conflictSeen[n][k] = TRUE,
                 !.fatal = @ \/ ~MutantIgnoreConflict,
                 !.badConflict = @ \/ MutantIgnoreConflict]

EVICT_NAMESPACE(n) ==
    LET eligible == NamespaceEligible(s, n)
        oldest == OldestEligible(s, n)
        next == ClearNamespace(s, n)
    IN /\ n \in Namespaces
       /\ eligible
       /\ ~s.evicted[n]
       /\ (oldest \/ MutantIgnoreLru)
       /\ s' = [next EXCEPT !.badLru = @ \/ ~oldest]

ADVANCE_GENERATION(n) ==
    LET wrapped == s.generation[n] = MaxGeneration
        nextGeneration == IF wrapped THEN 0 ELSE s.generation[n] + 1
    IN /\ n \in Namespaces
       /\ ~s.live[n]
       /\ s.generation[n] < MaxGeneration \/ MutantWrapGeneration
       /\ s' = [s EXCEPT
                    !.generation[n] = nextGeneration,
                    !.generationSeen[n] = @ \cup {nextGeneration},
                    !.badGeneration = @ \/ wrapped]

WRAP_GENERATION(n) ==
    /\ n \in Namespaces
    /\ ~s.live[n]
    /\ s.generation[n] = MaxGeneration
    /\ ~s.admissionStopped[n]
    /\ s' = [s EXCEPT
                 !.admissionStopped[n] = TRUE,
                 !.evicted[n] = TRUE]

GUID_FLIP(n, newGuid) ==
    /\ n \in Namespaces
    /\ newGuid \in Guids
    /\ s.admissionStopped[n]
    /\ (newGuid \notin s.guidHistory[n] \/ MutantReuseGuid)
    /\ (MutantCrossNamespaceAlias \/
        (\A other \in Namespaces : other # n =>
             /\ s.guid[other] # newGuid
             /\ newGuid \notin s.guidHistory[other]))
    /\ s' = [s EXCEPT
                 !.guid[n] = newGuid,
                 !.guidHistory[n] = @ \cup {newGuid},
                 !.generation[n] = 0,
                 !.generationSeen[n] = {0},
                 !.admissionStopped[n] = FALSE,
                 !.evicted[n] = TRUE,
                 !.badGuidReuse = @ \/ newGuid \in s.guidHistory[n],
                 !.badCrossNamespaceGuid = @ \/
                     \E other \in Namespaces :
                         other # n /\
                         (s.guid[other] = newGuid \/
                          newGuid \in s.guidHistory[other])]

STALL_WRITER(n, k) ==
    /\ MutantStallWriter
    /\ n \in Namespaces
    /\ k \in Keys
    /\ WriterWorkEnabled(s, n, k)
    /\ ~s.writerBlocked[n][k]
    /\ s' = [s EXCEPT !.writerBlocked[n][k] = TRUE]

WATCHDOG_TICK(n, k) ==
    LET nextCount == s.watchdogCount[n][k] + 1
    IN /\ n \in Namespaces
       /\ k \in Keys
       /\ WriterWorkEnabled(s, n, k)
       /\ s.writerBlocked[n][k]
       /\ s.watchdogCount[n][k] < WatchdogLimit
       /\ s' = [s EXCEPT
                    !.watchdogCount[n][k] = nextCount,
                    !.watchdogExpired = @ \/ nextCount >= WatchdogLimit]

RawNext ==
    \/ \E n \in Namespaces : ADMIT_NAMESPACE(n)
    \/ \E n \in Namespaces : TOUCH_NAMESPACE(n)
    \/ \E n \in Namespaces : START_TU(n)
    \/ \E n \in Namespaces : FINISH_TU(n)
    \/ \E n \in Namespaces, k \in Keys, slot \in Slots, value \in Values :
           BEGIN_INSTALL(n, k, slot, value)
    \/ \E n \in Namespaces, k \in Keys, slot \in Slots, value \in Values :
           RETRY_INSTALL(n, k, slot, value)
    \/ \E n \in Namespaces, k \in Keys, slot \in Slots :
           PUBLISH_INSTALL(n, k, slot)
    \/ \E n \in Namespaces, k \in Keys : PIN_OBJECT(n, k)
    \/ \E n \in Namespaces, k \in Keys : UNPIN_OBJECT(n, k)
    \/ \E n \in Namespaces, k \in Keys, slot \in Slots :
           CRASH_MID_INSTALL(n, k, slot)
    \/ \E n \in Namespaces, k \in Keys, value \in Values :
           CONFLICTING_CONTENT(n, k, value)
    \/ \E n \in Namespaces : EVICT_NAMESPACE(n)
    \/ \E n \in Namespaces : ADVANCE_GENERATION(n)
    \/ \E n \in Namespaces : WRAP_GENERATION(n)
    \/ \E n \in Namespaces, newGuid \in Guids : GUID_FLIP(n, newGuid)
    \/ \E n \in Namespaces, k \in Keys : STALL_WRITER(n, k)
    \/ \E n \in Namespaces, k \in Keys : WATCHDOG_TICK(n, k)

Next ==
    /\ step < MaxSteps
    /\ RawNext
    /\ step' = step + 1

(* The step bound is part of this bounded TLC harness, not a product
   liveness claim. All required S3 witnesses and red mutants fit below it. *)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ s.live \in [Namespaces -> BOOLEAN]
    /\ s.evicted \in [Namespaces -> BOOLEAN]
    /\ s.admissionCount \in [Namespaces -> 0..2]
    /\ s.guid \in [Namespaces -> Guids]
    /\ s.guidHistory \in [Namespaces -> SUBSET Guids]
    /\ s.generation \in [Namespaces -> Generations]
    /\ s.generationSeen \in [Namespaces -> SUBSET Generations]
    /\ s.admissionStopped \in [Namespaces -> BOOLEAN]
    /\ s.lru \in [Namespaces -> Nat]
    /\ s.clock \in Nat
    /\ s.touchCount \in [Namespaces -> Nat]
    /\ step \in 0..MaxSteps
    /\ s.active \in [Namespaces -> BOOLEAN]
    /\ s.tuUsed \in [Namespaces -> BOOLEAN]
    /\ s.freshAdmissionPending \in [Namespaces -> BOOLEAN]
    /\ s.arena \in [Namespaces -> [Keys -> ObjectStates]]
    /\ s.content \in [Namespaces -> [Keys -> Values \cup {NoContent}]]
    /\ s.stagingContent \in [Namespaces -> [Keys -> Values \cup {NoContent}]]
    /\ s.arenaGuid \in [Namespaces -> [Keys -> Guids \cup {NoGuid}]]
    /\ s.arenaGeneration \in [Namespaces -> [Keys -> Generations]]
    /\ s.installAttempts \in [Namespaces -> [Keys -> 0..2]]
    /\ s.pinUsed \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.slotOwner \in [Slots -> (Namespaces \X Keys) \cup {NoOwnerValue}]
    /\ s.crashed \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.retrySeen \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.conflictSeen \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.writerBlocked \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.watchdogCount \in [Namespaces -> [Keys -> 0..WatchdogLimit]]
    /\ s.watchdogExpired \in BOOLEAN
    /\ s.fatal \in BOOLEAN
    /\ s.badAggregate \in BOOLEAN
    /\ s.badNamespaceCap \in BOOLEAN
    /\ s.badSlot \in BOOLEAN
    /\ s.badLru \in BOOLEAN
    /\ s.badGeneration \in BOOLEAN
    /\ s.badAdmission \in BOOLEAN
    /\ s.badGuidReuse \in BOOLEAN
    /\ s.badConflict \in BOOLEAN
    /\ s.badCrash \in BOOLEAN
    /\ s.badContent \in BOOLEAN

AggregateByteCap == TotalBytes(s) <= MaxAggregateBytes

NamespaceByteCaps ==
    \A n \in Namespaces : NsBytes(s, n) <= MaxNamespaceBytes

StagingByteCap == TotalStagingBytes(s) <= MaxStagingBytes

TotalSimultaneousByteCap == TotalSimultaneousBytes(s) <= MaxTotalBytes

SlotOwners(st, slot) ==
    {owner \in (Namespaces \X Keys) :
        st.slotOwner[slot] = owner}

StagingSlotExclusive ==
    \A slot \in Slots :
        Cardinality(SlotOwners(s, slot)) <= 1

EveryOwnedSlotHasInstallingArena ==
    \A slot \in Slots :
        s.slotOwner[slot] = NoOwnerValue \/
            \E n \in Namespaces, k \in Keys :
                /\ s.slotOwner[slot] = Owner(n, k)
                /\ s.arena[n][k] = "INSTALLING"

InstallingOwnsExactlyOneSlot ==
    \A n \in Namespaces, k \in Keys :
        s.arena[n][k] = "INSTALLING" <=>
            Cardinality({slot \in Slots :
                         s.slotOwner[slot] = Owner(n, k)}) = 1

ArenaStateMachine ==
    \A n \in Namespaces, k \in Keys :
        /\ s.arena[n][k] = "ABSENT" =>
               /\ s.content[n][k] = NoContent
               /\ s.stagingContent[n][k] = NoContent
               /\ s.arenaGuid[n][k] = NoGuid
        /\ s.arena[n][k] = "INSTALLING" =>
               /\ s.stagingContent[n][k] \in Values
               /\ s.arenaGuid[n][k] = s.guid[n]
               /\ s.arenaGeneration[n][k] = s.generation[n]
        /\ s.arena[n][k] \in {"PRESENT", "PINNED"} =>
               /\ s.content[n][k] = CanonicalValue(n, k)
               /\ s.stagingContent[n][k] = NoContent
               /\ s.arenaGuid[n][k] = s.guid[n]
               /\ s.arenaGeneration[n][k] = s.generation[n]

ImmutableArenaContent ==
    \A n \in Namespaces, k \in Keys :
        s.arena[n][k] \in {"PRESENT", "PINNED"} =>
            /\ s.content[n][k] = CanonicalValue(n, k)
            /\ s.arenaGuid[n][k] = s.guid[n]
            /\ s.arenaGeneration[n][k] = s.generation[n]

LruOrderingWitness ==
    \A n, other \in Namespaces :
        n # other /\ s.touchCount[n] > 0 /\ s.touchCount[other] > 0 =>
            s.lru[n] # s.lru[other]

FreshAdmissionReady ==
    \A n \in Namespaces : s.freshAdmissionPending[n] =>
        /\ s.live[n]
        /\ ~s.active[n]
        /\ ~s.tuUsed[n]
        /\ (\A k \in Keys :
             /\ s.installAttempts[n][k] = 0
             /\ ~s.pinUsed[n][k]
             /\ ~s.crashed[n][k]
             /\ ~s.retrySeen[n][k]
             /\ ~s.conflictSeen[n][k])

CrossNamespaceGuidIsolation ==
    \A n, other \in Namespaces : n # other => s.guid[n] # s.guid[other]

CrossNamespaceGuidHistoryIsolation ==
    \A n, other \in Namespaces : n # other =>
        s.guidHistory[n] \cap s.guidHistory[other] = {}

ConflictIsFatal ==
    \A n \in Namespaces, k \in Keys :
        s.conflictSeen[n][k] => s.fatal

WholeNamespaceEviction ==
    \A n \in Namespaces :
        s.evicted[n] /\ ~s.live[n] =>
            /\ (\A k \in Keys : s.arena[n][k] = "ABSENT")
            /\ NsBytes(s, n) = 0
            /\ (\A slot \in Slots :
                   s.slotOwner[slot] # NoOwnerValue =>
                       s.slotOwner[slot][1] # n)

GenerationAdmissionStop ==
    \A n \in Namespaces :
        s.admissionStopped[n] =>
            /\ s.generation[n] = MaxGeneration
            /\ ~s.live[n]

GenerationNeverWraps ==
    \A n \in Namespaces :
        Cardinality(s.generationSeen[n]) = 1 + s.generation[n]

GuidFlipIsFresh ==
    \A n \in Namespaces : s.guid[n] \in s.guidHistory[n]

CrashMidInstallIsIdempotent ==
    \A n \in Namespaces, k \in Keys :
        s.crashed[n][k] =>
            /\ s.content[n][k] \in {NoContent, CanonicalValue(n, k)}
            /\ (s.arena[n][k] # "ABSENT" =>
                   s.arena[n][k] = "INSTALLING" \/
                   s.arena[n][k] \in {"PRESENT", "PINNED"})

NoMutantFaults ==
    /\ ~s.badAggregate
    /\ ~s.badNamespaceCap
    /\ ~s.badSlot
    /\ ~s.badLru
    /\ ~s.badGeneration
    /\ ~s.badAdmission
    /\ ~s.badGuidReuse
    /\ ~s.badConflict
    /\ ~s.badCrash
    /\ ~s.badContent
    /\ ~s.badCrossNamespaceGuid

WatchdogNoStall == ~s.watchdogExpired

THEOREM Spec => []TypeOK

=============================================================================

