----------------------- MODULE Protocol50TransferConcurrency -----------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Bounded Stage-A source-admission model.  A transfer reservation and session
owner are keyed by (C store, F store, their current generations); profile is
only a route selector.  Codec bytes are abstracted to one exact raw-byte unit
per request.  The model deliberately contains no ordered W>1 pipeline.
Sequence 0 is used only as the unallocated sentinel; the first modeled
admission gets 1.  Values are therefore offset from production TU_SEQ, whose
first value is 0; uniqueness and monotonic allocation are the modeled claims.
***************************************************************************)

CONSTANTS CStores, FStores, Profiles,
          MaxCCount, MaxCBytes, MaxFCount, MaxFBytes,
          ByteHeavyEnabled, HeavyF, OldProfile,
          BlockedC, BlockedF, ProgressC, ProgressF,
          ProgressMode,
          EnableFaultScenario, MutantProfileKey, MutantGlobalGate,
          MutantStaleCallback, MutantWrongCreditRelease,
          MutantDuplicateRelease, MutantEarlyLostAckRelease

ASSUME /\ CStores # {}
       /\ FStores # {}
       /\ Profiles # {}
       /\ MaxCCount \in Nat \ {0}
       /\ MaxCBytes \in Nat \ {0}
       /\ MaxFCount \in Nat \ {0}
       /\ MaxFBytes \in Nat \ {0}
       /\ OldProfile \in Profiles
       /\ ByteHeavyEnabled \in BOOLEAN
       /\ HeavyF \in FStores
       /\ BlockedC \in CStores
       /\ BlockedF \in FStores
       /\ ProgressC \in CStores
       /\ ProgressF \in FStores
       /\ ProgressMode \in BOOLEAN
       /\ EnableFaultScenario \in BOOLEAN
       /\ MutantProfileKey \in BOOLEAN
       /\ MutantGlobalGate \in BOOLEAN
       /\ MutantStaleCallback \in BOOLEAN
       /\ MutantWrongCreditRelease \in BOOLEAN
       /\ MutantDuplicateRelease \in BOOLEAN
       /\ MutantEarlyLostAckRelease \in BOOLEAN

Links == CStores \X FStores
Ops == { [c |-> c, f |-> f, p |-> p] :
         c \in CStores, f \in FStores, p \in Profiles }
LinkOf(o) == <<o.c, o.f>>
OpAt(l, p) == [c |-> l[1], f |-> l[2], p |-> p]
ProgressOp == OpAt(<<ProgressC, ProgressF>>, CHOOSE p \in Profiles : TRUE)
BlockedLink == <<BlockedC, BlockedF>>
NewProfile == CHOOSE p \in Profiles : p # OldProfile

IdleStates == {"Queued", "Committed", "Cancelled", "TimedOut", "Stopped"}
OwnedStates == {"Reserved", "Published", "Running", "ResponseReady", "CommittedUnacked"}
TerminalStates == {"Committed", "Cancelled", "TimedOut", "Stopped"}
TokenDomain == (0..2) \X (0..2) \X (0..2)
NoToken == <<2, 2, 2>>
NoSeq == 0

VARIABLES state, cGeneration, fGeneration, linkGeneration,
          owner, sequence, nextSequence, committedInput,
          cCount, cBytes, fCount, fBytes, releaseCount,
          lateCallback, staleMutation, stopping,
          compilerRestarted, committedAtRestart

vars == <<state, cGeneration, fGeneration, linkGeneration,
          owner, sequence, nextSequence, committedInput,
          cCount, cBytes, fCount, fBytes, releaseCount,
          lateCallback, staleMutation, stopping,
          compilerRestarted, committedAtRestart>>

CurrentToken(o) == <<cGeneration[o.c], fGeneration[o.f],
                    linkGeneration[LinkOf(o)]>>
LinkActive(l) == \E p \in Profiles : state[OpAt(l, p)] \in OwnedStates
AllLinksActive == \A l \in Links : LinkActive(l)
LinkRunning(l) == \E p \in Profiles : state[OpAt(l, p)] = "Running"
AllLinksRunning == \A l \in Links : LinkRunning(l)
OwnedForC(c) == {o \in Ops : o.c = c /\ state[o] \in OwnedStates}
OwnedForF(f) == {o \in Ops : o.f = f /\ state[o] \in OwnedStates}
UsedC(c) == Cardinality(OwnedForC(c))
UsedF(f) == Cardinality(OwnedForF(f))
RawUnit(o) == IF ByteHeavyEnabled /\ o.f = HeavyF THEN 2 ELSE 1
RECURSIVE ByteSum(_)
ByteSum(S) ==
    IF S = {} THEN 0
    ELSE LET o == CHOOSE x \in S : TRUE
         IN RawUnit(o) + ByteSum(S \ {o})
ByteUsageC(c) == ByteSum(OwnedForC(c))
ByteUsageF(f) == ByteSum(OwnedForF(f))

Init ==
    /\ state = [o \in Ops |-> "Queued"]
    /\ cGeneration = [c \in CStores |-> 0]
    /\ fGeneration = [f \in FStores |-> 0]
    /\ linkGeneration = [l \in Links |-> 0]
    /\ owner = [o \in Ops |-> NoToken]
    /\ sequence = [o \in Ops |-> NoSeq]
    /\ nextSequence = [c \in CStores |-> 1]
    /\ committedInput = {}
    /\ cCount = [c \in CStores |-> 0]
    /\ cBytes = [c \in CStores |-> 0]
    /\ fCount = [f \in FStores |-> 0]
    /\ fBytes = [f \in FStores |-> 0]
    /\ releaseCount = [o \in Ops |-> 0]
    /\ lateCallback = [l \in Links |-> NoToken]
    /\ staleMutation = FALSE
    /\ stopping = FALSE
    /\ compilerRestarted = [c \in CStores |-> FALSE]
    /\ committedAtRestart = [c \in CStores |-> {}]

CanAdmit(o) ==
    LET l == LinkOf(o)
        sameLink == {p \in Profiles : state[OpAt(l, p)] \in OwnedStates}
       globallyOwned == {x \in Ops : state[x] \in OwnedStates}
    IN /\ state[o] = "Queued"
       /\ IF MutantProfileKey
             THEN TRUE
             ELSE sameLink = {}
       /\ IF MutantGlobalGate
             THEN globallyOwned = {}
             ELSE TRUE
       /\ ~stopping
       /\ nextSequence[o.c] < Cardinality(FStores) * Cardinality(Profiles) + 1
       /\ cCount[o.c] < MaxCCount
       /\ cBytes[o.c] + RawUnit(o) <= MaxCBytes
       /\ fCount[o.f] < MaxFCount
       /\ fBytes[o.f] + RawUnit(o) <= MaxFBytes
       /\ IF EnableFaultScenario
             THEN IF o.p = OldProfile
                     THEN cGeneration[o.c] = 0 /\ fGeneration[o.f] = 0
                          /\ lateCallback[LinkOf(o)] = NoToken
                     ELSE cGeneration[o.c] = 1 \/ fGeneration[o.f] = 1
             ELSE TRUE

Admit(o) ==
    /\ CanAdmit(o)
    /\ sequence' = [sequence EXCEPT ![o] = nextSequence[o.c]]
    /\ nextSequence' = [nextSequence EXCEPT ![o.c] = @ + 1]
    /\ state' = [state EXCEPT ![o] = "Reserved"]
    /\ cCount' = [cCount EXCEPT ![o.c] = @ + 1]
    /\ cBytes' = [cBytes EXCEPT ![o.c] = @ + RawUnit(o)]
    /\ fCount' = [fCount EXCEPT ![o.f] = @ + 1]
    /\ fBytes' = [fBytes EXCEPT ![o.f] = @ + RawUnit(o)]
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    owner, committedInput, releaseCount,
                    lateCallback, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

CanPublish(o) ==
    /\ state[o] = "Reserved"
    /\ ~stopping
    /\ IF EnableFaultScenario
          THEN IF cGeneration[o.c] = 0 /\ fGeneration[o.f] = 0
                  THEN o.p = OldProfile ELSE o.p = NewProfile
          ELSE TRUE

Publish(o) ==
    /\ CanPublish(o)
    /\ state' = [state EXCEPT ![o] = "Published"]
    /\ owner' = [owner EXCEPT ![o] = CurrentToken(o)]
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    sequence, nextSequence, committedInput,
                    cCount, cBytes, fCount, fBytes, releaseCount,
                    lateCallback, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

BeginTransfer(o) ==
    /\ state[o] = "Published"
    /\ owner[o] = CurrentToken(o)
    /\ state' = [state EXCEPT ![o] = "Running"]
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    owner, sequence, nextSequence, committedInput,
                    cCount, cBytes, fCount, fBytes, releaseCount,
                    lateCallback, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

(***************************************************************************
PeerRespond is the environment response event. In the healthy-progress
scenario the designated blocked link may remain silent forever; every other
continuously running request has a response under weak fairness. A received
response is separate from the local commit observation/scheduler hand-off.
***************************************************************************)
PeerRespond(o) ==
    /\ ProgressMode
    /\ state[o] = "Running"
    /\ LinkOf(o) # BlockedLink
    /\ state' = [state EXCEPT ![o] = "ResponseReady"]
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    owner, sequence, nextSequence, committedInput,
                    cCount, cBytes, fCount, fBytes, releaseCount,
                    lateCallback, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

ReleaseReservation(o) ==
    /\ cCount[o.c] > 0
    /\ cBytes[o.c] >= RawUnit(o)
    /\ fCount[o.f] > 0
    /\ fBytes[o.f] >= RawUnit(o)
    /\ cCount' = [cCount EXCEPT ![o.c] = @ - 1]
    /\ cBytes' = [cBytes EXCEPT ![o.c] = @ - RawUnit(o)]
    /\ fCount' = [fCount EXCEPT ![o.f] = @ - 1]
    /\ fBytes' = [fBytes EXCEPT ![o.f] = @ - RawUnit(o)]
    /\ releaseCount' = [releaseCount EXCEPT ![o] = @ + 1]

CommitObserved(o) ==
    /\ state[o] = IF ProgressMode THEN "ResponseReady" ELSE "Running"
    /\ IF EnableFaultScenario
          THEN cGeneration[o.c] = 1 \/ fGeneration[o.f] = 1
          ELSE TRUE
    /\ IF BlockedLink # <<ProgressC, ProgressF>>
          THEN LinkOf(o) # BlockedLink ELSE TRUE
    /\ committedInput' = committedInput \cup {o}
    /\ state' = [state EXCEPT ![o] = "Committed"]
    /\ owner' = [owner EXCEPT ![o] = NoToken]
    /\ ReleaseReservation(o)
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    sequence, nextSequence, lateCallback,
                    staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

CommitLostAck(o) ==
    /\ ~ProgressMode
    /\ state[o] = "Running"
    /\ IF EnableFaultScenario
          THEN cGeneration[o.c] = 1 \/ fGeneration[o.f] = 1
          ELSE TRUE
    /\ IF BlockedLink # <<ProgressC, ProgressF>>
          THEN LinkOf(o) # BlockedLink ELSE TRUE
    /\ committedInput' = committedInput \cup {o}
    /\ state' = [state EXCEPT ![o] = "CommittedUnacked"]
    /\ IF MutantEarlyLostAckRelease
          THEN /\ owner' = [owner EXCEPT ![o] = NoToken]
               /\ ReleaseReservation(o)
          ELSE /\ owner' = [owner EXCEPT ![o] = CurrentToken(o)]
               /\ UNCHANGED <<cCount, cBytes, fCount, fBytes, releaseCount>>
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    sequence, nextSequence, lateCallback,
                    staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

ReconcileLostAck(o) ==
    /\ ~ProgressMode
    /\ state[o] = "CommittedUnacked"
    /\ owner[o] = CurrentToken(o)
    /\ state' = [state EXCEPT ![o] = "Committed"]
    /\ owner' = [owner EXCEPT ![o] = NoToken]
    /\ ReleaseReservation(o)
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    sequence, nextSequence, committedInput,
                    lateCallback, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

ExpireBeforeCommit(o) ==
    /\ EnableFaultScenario
    /\ state[o] \in {"Reserved", "Published", "Running"}
    /\ linkGeneration[LinkOf(o)] = 0
    /\ LET oldToken == IF owner[o] = NoToken THEN CurrentToken(o) ELSE owner[o]
           l == LinkOf(o)
       IN /\ state' = [state EXCEPT ![o] = "TimedOut"]
          /\ owner' = [owner EXCEPT ![o] = NoToken]
          /\ linkGeneration' = [linkGeneration EXCEPT ![l] = @ + 1]
          /\ lateCallback' = [lateCallback EXCEPT ![l] = oldToken]
    /\ ReleaseReservation(o)
    /\ UNCHANGED <<cGeneration, fGeneration, sequence, nextSequence,
                    committedInput, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

DeliverLateCallback(l) ==
    /\ EnableFaultScenario
    /\ lateCallback[l] # NoToken
    /\ IF (MutantStaleCallback \/ MutantWrongCreditRelease) /\
          (\E o \in Ops : LinkOf(o) = l /\ state[o] \in OwnedStates)
          THEN LET current == CHOOSE o \in Ops :
                              LinkOf(o) = l /\ state[o] \in OwnedStates
               IN /\ staleMutation' = staleMutation \/ MutantStaleCallback
                  /\ state' = IF MutantStaleCallback
                                 THEN [state EXCEPT ![current] = "Published"]
                                 ELSE state
                  /\ owner' = IF MutantStaleCallback
                                 THEN [owner EXCEPT ![current] = lateCallback[l]]
                                 ELSE owner
                  /\ IF MutantWrongCreditRelease
                        THEN /\ cCount' = [cCount EXCEPT ![current.c] = @ - 1]
                             /\ cBytes' = [cBytes EXCEPT ![current.c] = @ - RawUnit(current)]
                             /\ fCount' = [fCount EXCEPT ![current.f] = @ - 1]
                             /\ fBytes' = [fBytes EXCEPT ![current.f] = @ - RawUnit(current)]
                        ELSE UNCHANGED <<cCount, cBytes, fCount, fBytes>>
          ELSE /\ staleMutation' = staleMutation
               /\ UNCHANGED <<state, owner, cCount, cBytes, fCount, fBytes>>
    /\ lateCallback' = [lateCallback EXCEPT ![l] = NoToken]
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    sequence, nextSequence, committedInput, releaseCount,
                    stopping, compilerRestarted, committedAtRestart>>

RestartCompiler(c) ==
    /\ c \in CStores
    /\ ~ProgressMode
    /\ ~compilerRestarted[c]
    /\ compilerRestarted' = [compilerRestarted EXCEPT ![c] = TRUE]
    /\ committedAtRestart' = [committedAtRestart EXCEPT ![c] =
                                {o \in committedInput : o.c = c}]
    /\ UNCHANGED <<state, cGeneration, fGeneration, linkGeneration,
                    owner, sequence, nextSequence, committedInput,
                    cCount, cBytes, fCount, fBytes, releaseCount,
                    lateCallback, staleMutation, stopping>>

ReplaceFIncarnation(f, o) ==
    /\ EnableFaultScenario
    /\ f \in FStores
    /\ o \in Ops
    /\ o.f = f
    /\ fGeneration[f] = 0
    /\ state[o] \in {"Reserved", "Published", "Running", "ResponseReady"}
    /\ \A x \in Ops : x.f = f /\ state[x] \in OwnedStates => x = o
    /\ state' = [state EXCEPT ![o] = "TimedOut"]
    /\ owner' = [owner EXCEPT ![o] = NoToken]
    /\ fGeneration' = [fGeneration EXCEPT ![f] = 1]
    /\ lateCallback' = [lateCallback EXCEPT ![LinkOf(o)] = CurrentToken(o)]
    /\ ReleaseReservation(o)
    /\ UNCHANGED <<cGeneration, linkGeneration, sequence, nextSequence,
                    committedInput, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

ReplaceCNamespace(c, o) ==
    /\ EnableFaultScenario
    /\ c \in CStores
    /\ o \in Ops
    /\ o.c = c
    /\ cGeneration[c] = 0
    /\ state[o] \in {"Reserved", "Published", "Running", "ResponseReady"}
    /\ \A x \in Ops : x.c = c /\ state[x] \in OwnedStates => x = o
    /\ state' = [state EXCEPT ![o] = "TimedOut"]
    /\ owner' = [owner EXCEPT ![o] = NoToken]
    /\ cGeneration' = [cGeneration EXCEPT ![c] = 1]
    /\ lateCallback' = [lateCallback EXCEPT ![LinkOf(o)] = CurrentToken(o)]
    /\ ReleaseReservation(o)
    /\ UNCHANGED <<fGeneration, linkGeneration, sequence, nextSequence,
                    committedInput, staleMutation, stopping,
                    compilerRestarted, committedAtRestart>>

DuplicateRelease(o) ==
    /\ MutantDuplicateRelease
    /\ state[o] = "Committed"
    /\ releaseCount[o] = 1
    /\ releaseCount' = [releaseCount EXCEPT ![o] = 2]
    /\ UNCHANGED <<state, cGeneration, fGeneration, linkGeneration,
                    owner, sequence, nextSequence, committedInput,
                    cCount, cBytes, fCount, fBytes, lateCallback,
                    staleMutation, stopping, compilerRestarted,
                    committedAtRestart>>

Stop ==
    /\ EnableFaultScenario
    /\ ~stopping
    /\ stopping' = TRUE
    /\ state' = [o \in Ops |->
          IF state[o] = "CommittedUnacked" THEN state[o]
          ELSE IF state[o] \in OwnedStates \/ state[o] = "Queued"
                  THEN "Stopped" ELSE state[o]]
    /\ owner' = [o \in Ops |->
          IF state[o] = "CommittedUnacked" THEN owner[o] ELSE NoToken]
    /\ cCount' = [c \in CStores |-> Cardinality(
          {o \in Ops : o.c = c /\ state[o] = "CommittedUnacked"})]
    /\ cBytes' = [c \in CStores |-> ByteSum(
          {o \in Ops : o.c = c /\ state[o] = "CommittedUnacked"})]
    /\ fCount' = [f \in FStores |-> Cardinality(
          {o \in Ops : o.f = f /\ state[o] = "CommittedUnacked"})]
    /\ fBytes' = [f \in FStores |-> ByteSum(
          {o \in Ops : o.f = f /\ state[o] = "CommittedUnacked"})]
    /\ releaseCount' = [o \in Ops |->
          IF state[o] \in OwnedStates /\ state[o] # "CommittedUnacked"
             THEN releaseCount[o] + 1 ELSE releaseCount[o]]
    /\ UNCHANGED <<cGeneration, fGeneration, linkGeneration,
                    sequence, nextSequence, committedInput,
                    lateCallback, staleMutation,
                    compilerRestarted, committedAtRestart>>

Next ==
    \/ \E o \in Ops : Admit(o)
    \/ \E o \in Ops : Publish(o)
    \/ \E o \in Ops : BeginTransfer(o)
    \/ \E o \in Ops : PeerRespond(o)
    \/ \E o \in Ops : CommitObserved(o)
    \/ \E o \in Ops : CommitLostAck(o)
    \/ \E o \in Ops : ReconcileLostAck(o)
    \/ \E o \in Ops : ExpireBeforeCommit(o)
    \/ \E l \in Links : DeliverLateCallback(l)
    \/ \E c \in CStores : RestartCompiler(c)
    \/ \E f \in FStores, o \in Ops : ReplaceFIncarnation(f, o)
    \/ \E c \in CStores, o \in Ops : ReplaceCNamespace(c, o)
    \/ \E o \in Ops : DuplicateRelease(o)
    \/ IF EnableFaultScenario THEN Stop ELSE FALSE

Spec == Init /\ [][Next]_vars
        /\ \A admitOp \in Ops : WF_vars(Admit(admitOp))
        /\ \A publishOp \in Ops : WF_vars(Publish(publishOp))
        /\ \A transferOp \in Ops : WF_vars(BeginTransfer(transferOp))
        /\ \A observedOp \in Ops : WF_vars(CommitObserved(observedOp))
        /\ \A lostAckOp \in Ops : WF_vars(CommitLostAck(lostAckOp))
        /\ \A reconcileOp \in Ops : WF_vars(ReconcileLostAck(reconcileOp))
        /\ IF ProgressMode
              THEN \A responseOp \in Ops : WF_vars(PeerRespond(responseOp))
              ELSE TRUE
        /\ \A restartedC \in CStores : WF_vars(RestartCompiler(restartedC))
        /\ \A callbackLink \in Links : WF_vars(DeliverLateCallback(callbackLink))
        /\ \A replacedF \in FStores : WF_vars(\E fOp \in Ops : ReplaceFIncarnation(replacedF, fOp))
        /\ \A replacedC \in CStores : WF_vars(\E cOp \in Ops : ReplaceCNamespace(replacedC, cOp))

TypeOK ==
    /\ state \in [Ops -> (IdleStates \cup OwnedStates)]
    /\ cGeneration \in [CStores -> 0..1]
    /\ fGeneration \in [FStores -> 0..1]
    /\ linkGeneration \in [Links -> 0..1]
    /\ owner \in [Ops -> (TokenDomain \cup {NoToken})]
    /\ sequence \in [Ops -> 0..(Cardinality(FStores) * Cardinality(Profiles))]
    /\ nextSequence \in [CStores -> 1..(Cardinality(FStores) * Cardinality(Profiles) + 1)]
    /\ committedInput \subseteq Ops
    /\ cCount \in [CStores -> 0..MaxCCount]
    /\ cBytes \in [CStores -> 0..MaxCBytes]
    /\ fCount \in [FStores -> 0..MaxFCount]
    /\ fBytes \in [FStores -> 0..MaxFBytes]
    /\ releaseCount \in [Ops -> 0..2]
    /\ lateCallback \in [Links -> (TokenDomain \cup {NoToken})]
    /\ staleMutation \in BOOLEAN
    /\ stopping \in BOOLEAN
    /\ compilerRestarted \in [CStores -> BOOLEAN]
    /\ committedAtRestart \in [CStores -> SUBSET Ops]

OneOwnerPerExactRelationship ==
    \A l \in Links : Cardinality(
       {p \in Profiles : state[OpAt(l, p)] \in OwnedStates}) <= 1

OwnerPublicationMatchesGeneration ==
    \A o \in Ops :
       state[o] \in {"Published", "Running", "CommittedUnacked"}
         => owner[o] = CurrentToken(o)

ReservationCountsMatchOwnedOperations ==
    /\ \A c \in CStores :
          /\ cCount[c] = UsedC(c)
          /\ cBytes[c] = ByteUsageC(c)
    /\ \A f \in FStores :
          /\ fCount[f] = UsedF(f)
          /\ fBytes[f] = ByteUsageF(f)

PerStoreResourceBounds ==
    /\ \A c \in CStores : cCount[c] <= MaxCCount /\ cBytes[c] <= MaxCBytes
    /\ \A f \in FStores : fCount[f] <= MaxFCount /\ fBytes[f] <= MaxFBytes

AtMostOneConcurrentUnderBytePressure ==
    \A c \in CStores : cCount[c] <= 1

UniqueCWideTUSequence ==
    \A a, b \in Ops :
       (a # b /\ a.c = b.c /\ sequence[a] # NoSeq /\ sequence[b] # NoSeq)
          => sequence[a] # sequence[b]

CommittedUnobservedRetainsWitness ==
    \A o \in Ops : state[o] = "CommittedUnacked" =>
       /\ o \in committedInput
       /\ o \in OwnedForC(o.c)
       /\ o \in OwnedForF(o.f)
       /\ owner[o] = CurrentToken(o)

CommittedInputSurvivesCompilerRestart ==
    \A c \in CStores : committedAtRestart[c] \subseteq committedInput

LateCallbackCannotMutateCurrentOperation == ~staleMutation
ReservationReleasedAtMostOnce == \A o \in Ops : releaseCount[o] <= 1
StopPreventsAdmissionAndPublication ==
    stopping =>
       /\ \A o \in Ops : state[o] # "Queued"
       /\ \A o \in Ops : state[o] \notin {"Reserved"}

NoAllLinksRunning == ~AllLinksRunning
BlockedLinkDoesNotBlockHealthyProgress ==
    <> (state[OpAt(BlockedLink, CHOOSE p \in Profiles : TRUE)] = "Running"
        /\ state[ProgressOp] = "Committed")

=============================================================================
