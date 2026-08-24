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

The model is safety-scoped.  The bounded writer watchdog is represented as a
fail-closed safety condition (STALL_WRITER sets a fault); the product
progress claim must bind the same condition to its deterministic watchdog.
***************************************************************************)

CONSTANTS N0, N1, K0, K1, S0, S1,
          G0, G1, G2, NoGuid, NoKey, NoNamespace, NoOwner,
          NoContent, V0, V1,
          MaxGeneration, MaxAggregateBytes, MaxNamespaceBytes,
          MutantIgnoreAggregateCap,
          MutantIgnoreNamespaceCap,
          MutantIgnoreSlotOwnership,
          MutantIgnoreLru,
          MutantWrapGeneration,
          MutantAdmitWhileStopped,
          MutantReuseGuid,
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
       /\ MutantIgnoreAggregateCap \in BOOLEAN
       /\ MutantIgnoreNamespaceCap \in BOOLEAN
       /\ MutantIgnoreSlotOwnership \in BOOLEAN
       /\ MutantIgnoreLru \in BOOLEAN
       /\ MutantWrapGeneration \in BOOLEAN
       /\ MutantAdmitWhileStopped \in BOOLEAN
       /\ MutantReuseGuid \in BOOLEAN
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

CanonicalValue(k) == IF k = K0 THEN V0 ELSE V1
ObjectBytes(k) == IF k = K0 THEN 2 ELSE 3
Owner(n, k) == <<n, k>>

VARIABLE s
vars == <<s>>

Init ==
    s = [live                 |-> [n \in Namespaces |-> FALSE],
         evicted              |-> [n \in Namespaces |-> FALSE],
         guid                 |-> [n \in Namespaces |-> G0],
         guidHistory          |-> [n \in Namespaces |-> {G0}],
         generation           |-> [n \in Namespaces |-> 0],
         generationSeen       |-> [n \in Namespaces |-> {0}],
         admissionStopped     |-> [n \in Namespaces |-> FALSE],
         lru                  |-> [n \in Namespaces |-> 0],
         clock                |-> 0,
         active               |-> [n \in Namespaces |-> FALSE],
         arena                |-> [n \in Namespaces |->
                                      [k \in Keys |-> "ABSENT"]],
         content              |-> [n \in Namespaces |->
                                      [k \in Keys |-> NoContent]],
         stagingContent       |-> [n \in Namespaces |->
                                      [k \in Keys |-> NoContent]],
         slotOwner            |-> [slot \in Slots |-> NoOwnerValue],
         crashed              |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         retrySeen            |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
         conflictSeen         |-> [n \in Namespaces |->
                                      [k \in Keys |-> FALSE]],
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
         stalled               |-> FALSE]

NsBytes(st, n) ==
    Sum({IF st.arena[n][k] \in {"PRESENT", "PINNED"}
             THEN ObjectBytes(k) ELSE 0 : k \in Keys})

TotalBytes(st) == Sum({NsBytes(st, n) : n \in Namespaces})

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
        !.arena[n] = [k \in Keys |-> "ABSENT"],
        !.content[n] = [k \in Keys |-> NoContent],
        !.stagingContent[n] = [k \in Keys |-> NoContent],
        !.slotOwner = [slot \in Slots |->
                          IF st.slotOwner[slot][1] = n
                          THEN NoOwnerValue ELSE st.slotOwner[slot]]]

ADMIT_NAMESPACE(n) ==
    LET rejected == s.admissionStopped[n]
    IN /\ n \in Namespaces
       /\ ~s.live[n]
       /\ (~s.admissionStopped[n] \/ MutantAdmitWhileStopped)
       /\ s' = [s EXCEPT
                    !.live[n] = TRUE,
                    !.evicted[n] = FALSE,
                    !.badAdmission = @ \/ rejected]

TOUCH_NAMESPACE(n) ==
    /\ n \in Namespaces
    /\ s.live[n]
    /\ s' = [s EXCEPT
                 !.clock = @ + 1,
                 !.lru[n] = @ + 1]

START_TU(n) ==
    /\ n \in Namespaces
    /\ s.live[n]
    /\ ~s.active[n]
    /\ s' = [s EXCEPT !.active[n] = TRUE]

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
       /\ s.arena[n][k] = "ABSENT"
       /\ (s.slotOwner[slot] = NoOwnerValue \/ MutantIgnoreSlotOwnership)
       /\ s' = [s EXCEPT
                    !.arena[n][k] = "INSTALLING",
                    !.stagingContent[n][k] = value,
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
    /\ s.slotOwner[slot] = NoOwnerValue
    /\ s' = [s EXCEPT
                 !.arena[n][k] = "INSTALLING",
                 !.stagingContent[n][k] = value,
                 !.slotOwner[slot] = Owner(n, k),
                 !.retrySeen[n][k] = TRUE]

PUBLISH_INSTALL(n, k, slot) ==
    LET value == s.stagingContent[n][k]
        nextNamespaceBytes == NsBytes(s, n) + ObjectBytes(k)
        nextTotalBytes == TotalBytes(s) + ObjectBytes(k)
        canonical == value = CanonicalValue(k)
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
                        IF MutantBadContent THEN value ELSE CanonicalValue(k),
                    !.stagingContent[n][k] = NoContent,
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
    /\ s' = [s EXCEPT !.arena[n][k] = "PINNED"]

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
                 !.slotOwner[slot] =
                     IF MutantCrashKeepsSlot THEN @ ELSE NoOwnerValue,
                 !.crashed[n][k] = TRUE,
                 !.badCrash = @ \/ MutantCrashKeepsSlot]

CONFLICTING_CONTENT(n, k, value) ==
    /\ n \in Namespaces
    /\ k \in Keys
    /\ value \in Values
    /\ s.content[n][k] \in Values
    /\ value # s.content[n][k]
    /\ s.arena[n][k] \in {"PRESENT", "PINNED"}
    /\ s' = [s EXCEPT
                 !.conflictSeen[n][k] = TRUE,
                 !.fatal = TRUE \/ MutantIgnoreConflict,
                 !.badConflict = @ \/ MutantIgnoreConflict]

EVICT_NAMESPACE(n) ==
    LET eligible == NamespaceEligible(s, n)
        oldest == OldestEligible(s, n)
        next == ClearNamespace(s, n)
    IN /\ n \in Namespaces
       /\ eligible
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
    /\ s' = [s EXCEPT
                 !.guid[n] = newGuid,
                 !.guidHistory[n] = @ \cup {newGuid},
                 !.generation[n] = 0,
                 !.generationSeen[n] = {0},
                 !.admissionStopped[n] = FALSE,
                 !.evicted[n] = TRUE,
                 !.badGuidReuse = @ \/ newGuid \in s.guidHistory[n]]

STALL_WRITER(n, k) ==
    /\ MutantStallWriter
    /\ n \in Namespaces
    /\ k \in Keys
    /\ s.arena[n][k] = "INSTALLING"
    /\ s' = [s EXCEPT !.stalled = TRUE]

Next ==
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

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ s.live \in [Namespaces -> BOOLEAN]
    /\ s.evicted \in [Namespaces -> BOOLEAN]
    /\ s.guid \in [Namespaces -> Guids]
    /\ s.guidHistory \in [Namespaces -> SUBSET Guids]
    /\ s.generation \in [Namespaces -> Generations]
    /\ s.generationSeen \in [Namespaces -> SUBSET Generations]
    /\ s.admissionStopped \in [Namespaces -> BOOLEAN]
    /\ s.lru \in [Namespaces -> Nat]
    /\ s.clock \in Nat
    /\ s.active \in [Namespaces -> BOOLEAN]
    /\ s.arena \in [Namespaces -> [Keys -> ObjectStates]]
    /\ s.content \in [Namespaces -> [Keys -> Values \cup {NoContent}]]
    /\ s.stagingContent \in [Namespaces -> [Keys -> Values \cup {NoContent}]]
    /\ s.slotOwner \in [Slots -> (Namespaces \X Keys) \cup {NoOwnerValue}]
    /\ s.crashed \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.retrySeen \in [Namespaces -> [Keys -> BOOLEAN]]
    /\ s.conflictSeen \in [Namespaces -> [Keys -> BOOLEAN]]
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
    /\ s.stalled \in BOOLEAN

AggregateByteCap == TotalBytes(s) <= MaxAggregateBytes

NamespaceByteCaps ==
    \A n \in Namespaces : NsBytes(s, n) <= MaxNamespaceBytes

StagingSlotExclusive ==
    \A slot \in Slots :
        Cardinality({Owner(n, k) : n \in Namespaces, k \in Keys,
                     s.slotOwner[slot] = Owner(n, k)}) <= 1

InstallingOwnsExactlyOneSlot ==
    \A n \in Namespaces, k \in Keys :
        s.arena[n][k] = "INSTALLING" <=>
            Cardinality({slot \in Slots :
                         s.slotOwner[slot] = Owner(n, k)}) = 1

ArenaStateMachine ==
    \A n \in Namespaces, k \in Keys :
        /\ s.arena[n][k] = "ABSENT" =>
               /\ s.content[n][k] = NoContent
               /\ s.stagingContent[n][k] = NoContent \/
                   s.stagingContent[n][k] \in Values
        /\ s.arena[n][k] = "INSTALLING" =>
               s.stagingContent[n][k] \in Values
        /\ s.arena[n][k] \in {"PRESENT", "PINNED"} =>
               /\ s.content[n][k] = CanonicalValue(k)
               /\ s.stagingContent[n][k] = NoContent

ImmutableArenaContent ==
    \A n \in Namespaces, k \in Keys :
        s.arena[n][k] \in {"PRESENT", "PINNED"} =>
            s.content[n][k] = CanonicalValue(k)

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
            /\ s.content[n][k] \in {NoContent, CanonicalValue(k)}
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

WatchdogNoStall == ~s.stalled

THEOREM Spec => []TypeOK

=============================================================================
