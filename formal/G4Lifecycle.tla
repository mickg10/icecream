------------------------------ MODULE G4Lifecycle ------------------------------
(***************************************************************************
Small composed model for the issue-4 daemon/session and count>1 batch path.
It models the states present at product revision a34e825, plus the smallest
required corrections.  It does not introduce protocol 49/50.

The fixed model checks:
  * activation for legacy protocol 21..23 and ConfCS protocol >=24;
  * bounded batch admission and exact accepted-entry accounting;
  * local slot ownership and JobBegin-before-LOCAL_STARTED;
  * fail-closed cleanup for a non-owned batch-local CompileFile;
  * session-loss quiescence; and
  * lexicographic (niceness, client_id) local selection.

Each Boolean mutant changes one premise and has one dedicated TLC row.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Protocol,
          MaxBatch,
          Capacity,
          MutantLegacyNeedsConf,
          MutantStartBeforeBegin,
          MutantKeepRejectedSlot,
          MutantUnboundedBatch,
          MutantConjPriority

ASSUME /\ Protocol \in Nat
       /\ MaxBatch \in 1..3
       /\ Capacity \in 1..3
       /\ MutantLegacyNeedsConf \in BOOLEAN
       /\ MutantStartBeforeBegin \in BOOLEAN
       /\ MutantKeepRejectedSlot \in BOOLEAN
       /\ MutantUnboundedBatch \in BOOLEAN
       /\ MutantConjPriority \in BOOLEAN

Clients == {"c0", "c1"}
NoClient == "none"
ClientId == [c \in Clients |-> IF c = "c0" THEN 1 ELSE 2]
Nice == [c \in Clients |-> IF c = "c0" THEN 10 ELSE 0]

Decisions == {"d0", "d1", "d2"}
DecisionNo ==
    [d \in Decisions |->
        IF d = "d0" THEN 1
        ELSE IF d = "d1" THEN 2
        ELSE 3]

Sessions == {"Disconnected", "LoginAttempt", "Active"}
RequestStates == {"Idle", "Waiting", "Closed"}
DecisionPhases == {
    "Absent",
    "RemoteDelivered",
    "LocalWaiting",
    "LocalBound",
    "LocalDelivered",
    "LocalStarted",
    "Terminal"
}
LocalLivePhases == {"LocalWaiting", "LocalBound", "LocalDelivered", "LocalStarted"}
LivePhases == LocalLivePhases \cup {"RemoteDelivered"}
SlotPhases == {"LocalDelivered", "LocalStarted"}
AcceptedPhases == DecisionPhases \ {"Absent"}

Events == {
    "Init", "LoginSent", "LegacyActivated", "ConfActivated", "ZeroNoop",
    "BatchAccepted", "BatchOverflowRejected", "LocalAccepted",
    "RemoteAccepted", "LocalBound", "LocalDelivered", "BeginCommitted",
    "BeginSendFailed", "NonOwnedArmed", "RejectNonOwned", "Completed",
    "RejectWrongDone", "RejectDuplicateDone", "SessionLost",
    "SessionCleaned", "ClientSelected"
}
CountChoices == {0, 1, MaxBatch, MaxBatch + 1}

VARIABLES session,
          generation,
          confSeen,
          requestState,
          expected,
          accepted,
          phase,
          ownerGen,
          slotCharged,
          beginCommitted,
          terminalCount,
          forcedNonOwned,
          lossPending,
          selected,
          lastEvent

vars == <<session, generation, confSeen, requestState, expected, accepted,
          phase, ownerGen, slotCharged, beginCommitted, terminalCount,
          forcedNonOwned, lossPending, selected, lastEvent>>

TypeOK ==
    /\ session \in Sessions
    /\ generation \in 0..1
    /\ confSeen \in BOOLEAN
    /\ requestState \in RequestStates
    /\ expected \in 0..(MaxBatch + 1)
    /\ accepted \in 0..Cardinality(Decisions)
    /\ phase \in [Decisions -> DecisionPhases]
    /\ ownerGen \in [Decisions -> 0..1]
    /\ slotCharged \in [Decisions -> BOOLEAN]
    /\ beginCommitted \in [Decisions -> BOOLEAN]
    /\ terminalCount \in [Decisions -> 0..1]
    /\ forcedNonOwned \in [Decisions -> BOOLEAN]
    /\ lossPending \in BOOLEAN
    /\ selected \in Clients \cup {NoClient}
    /\ lastEvent \in Events

Init ==
    /\ session = "Disconnected"
    /\ generation = 0
    /\ confSeen = FALSE
    /\ requestState = "Idle"
    /\ expected = 0
    /\ accepted = 0
    /\ phase = [d \in Decisions |-> "Absent"]
    /\ ownerGen = [d \in Decisions |-> 0]
    /\ slotCharged = [d \in Decisions |-> FALSE]
    /\ beginCommitted = [d \in Decisions |-> FALSE]
    /\ terminalCount = [d \in Decisions |-> 0]
    /\ forcedNonOwned = [d \in Decisions |-> FALSE]
    /\ lossPending = FALSE
    /\ selected = NoClient
    /\ lastEvent = "Init"

SlotOccupancy == Cardinality({d \in Decisions : slotCharged[d]})
AcceptedSet == {d \in Decisions : phase[d] \in AcceptedPhases}

Connect ==
    /\ session = "Disconnected"
    /\ generation = 0
    /\ ~lossPending
    /\ session' = "LoginAttempt"
    /\ confSeen' = FALSE
    /\ lastEvent' = "LoginSent"
    /\ UNCHANGED <<generation, requestState, expected, accepted, phase,
                    ownerGen, slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

ActivateLegacy ==
    /\ session = "LoginAttempt"
    /\ Protocol < 24
    /\ ~MutantLegacyNeedsConf
    /\ session' = "Active"
    /\ generation' = generation + 1
    /\ lastEvent' = "LegacyActivated"
    /\ UNCHANGED <<confSeen, requestState, expected, accepted, phase,
                    ownerGen, slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

ReceiveConf ==
    /\ session = "LoginAttempt"
    /\ Protocol >= 24
    /\ session' = "Active"
    /\ generation' = generation + 1
    /\ confSeen' = TRUE
    /\ lastEvent' = "ConfActivated"
    /\ UNCHANGED <<requestState, expected, accepted, phase, ownerGen,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

SubmitZero ==
    /\ session = "Active"
    /\ requestState = "Idle"
    /\ lastEvent' = "ZeroNoop"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, phase, ownerGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, lossPending, selected>>

SubmitValid(n) ==
    /\ n \in CountChoices \ {0}
    /\ n <= MaxBatch
    /\ session = "Active"
    /\ requestState = "Idle"
    /\ requestState' = "Waiting"
    /\ expected' = n
    /\ lastEvent' = "BatchAccepted"
    /\ UNCHANGED <<session, generation, confSeen, accepted, phase,
                    ownerGen, slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

SubmitOverflow(n) ==
    /\ n \in CountChoices
    /\ n > MaxBatch
    /\ session = "Active"
    /\ requestState = "Idle"
    /\ IF MutantUnboundedBatch
          THEN /\ requestState' = "Waiting"
               /\ expected' = n
               /\ lastEvent' = "BatchAccepted"
          ELSE /\ requestState' = "Closed"
               /\ expected' = 0
               /\ lastEvent' = "BatchOverflowRejected"
    /\ UNCHANGED <<session, generation, confSeen, accepted, phase,
                    ownerGen, slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

AcceptLocal(d) ==
    /\ d \in Decisions
    /\ session = "Active"
    /\ requestState = "Waiting"
    /\ accepted < expected
    /\ DecisionNo[d] <= expected
    /\ phase[d] = "Absent"
    /\ phase' = [phase EXCEPT ![d] = "LocalWaiting"]
    /\ ownerGen' = [ownerGen EXCEPT ![d] = generation]
    /\ accepted' = accepted + 1
    /\ lastEvent' = "LocalAccepted"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

AcceptRemote(d) ==
    /\ d \in Decisions
    /\ session = "Active"
    /\ requestState = "Waiting"
    /\ accepted < expected
    /\ DecisionNo[d] <= expected
    /\ phase[d] = "Absent"
    /\ phase' = [phase EXCEPT ![d] = "RemoteDelivered"]
    /\ ownerGen' = [ownerGen EXCEPT ![d] = generation]
    /\ accepted' = accepted + 1
    /\ lastEvent' = "RemoteAccepted"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

BindLocal(d) ==
    /\ d \in Decisions
    /\ phase[d] = "LocalWaiting"
    /\ phase' = [phase EXCEPT ![d] = "LocalBound"]
    /\ lastEvent' = "LocalBound"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, ownerGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, lossPending, selected>>

DeliverLocal(d) ==
    /\ d \in Decisions
    /\ phase[d] = "LocalBound"
    /\ SlotOccupancy < Capacity
    /\ phase' = [phase EXCEPT ![d] = "LocalDelivered"]
    /\ slotCharged' = [slotCharged EXCEPT ![d] = TRUE]
    /\ lastEvent' = "LocalDelivered"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, ownerGen, beginCommitted, terminalCount,
                    forcedNonOwned, lossPending, selected>>

CommitBegin(d) ==
    /\ d \in Decisions
    /\ session = "Active"
    /\ phase[d] = "LocalDelivered"
    /\ ownerGen[d] = generation
    /\ ~forcedNonOwned[d]
    /\ phase' = [phase EXCEPT ![d] = "LocalStarted"]
    /\ beginCommitted' = [beginCommitted EXCEPT ![d] = TRUE]
    /\ lastEvent' = "BeginCommitted"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, ownerGen, slotCharged, terminalCount,
                    forcedNonOwned, lossPending, selected>>

FailBegin(d) ==
    /\ d \in Decisions
    /\ session = "Active"
    /\ phase[d] = "LocalDelivered"
    /\ ownerGen[d] = generation
    /\ ~forcedNonOwned[d]
    /\ phase' = [phase EXCEPT
          ![d] = IF MutantStartBeforeBegin THEN "LocalStarted" ELSE @]
    /\ session' = "Disconnected"
    /\ confSeen' = FALSE
    /\ lossPending' = TRUE
    /\ lastEvent' = "BeginSendFailed"
    /\ UNCHANGED <<generation, requestState, expected, accepted, ownerGen,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, selected>>

ArmNonOwned(d) ==
    /\ d \in Decisions
    /\ phase[d] = "LocalDelivered"
    /\ \A x \in Decisions : ~forcedNonOwned[x]
    /\ forcedNonOwned' = [forcedNonOwned EXCEPT ![d] = TRUE]
    /\ lastEvent' = "NonOwnedArmed"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, phase, ownerGen, slotCharged, beginCommitted,
                    terminalCount, lossPending, selected>>

RejectNonOwned(d) ==
    /\ d \in Decisions
    /\ phase[d] = "LocalDelivered"
    /\ forcedNonOwned[d]
    /\ phase' = [phase EXCEPT
          ![d] = IF MutantKeepRejectedSlot THEN @ ELSE "Terminal"]
    /\ slotCharged' = [slotCharged EXCEPT
          ![d] = IF MutantKeepRejectedSlot THEN @ ELSE FALSE]
    /\ terminalCount' = [terminalCount EXCEPT
          ![d] = IF MutantKeepRejectedSlot THEN @ ELSE @ + 1]
    /\ requestState' = IF MutantKeepRejectedSlot THEN requestState ELSE "Closed"
    /\ lastEvent' = "RejectNonOwned"
    /\ UNCHANGED <<session, generation, confSeen, expected, accepted,
                    ownerGen, beginCommitted, forcedNonOwned, lossPending,
                    selected>>

WrongDone(d) ==
    /\ d \in Decisions
    /\ phase[d] \in {"LocalWaiting", "LocalBound", "LocalDelivered"}
    /\ lastEvent' = "RejectWrongDone"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, phase, ownerGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, lossPending, selected>>

CompleteDecision(d) ==
    /\ d \in Decisions
    /\ phase[d] \in {"RemoteDelivered", "LocalStarted"}
    /\ phase' = [phase EXCEPT ![d] = "Terminal"]
    /\ slotCharged' = [slotCharged EXCEPT ![d] = FALSE]
    /\ terminalCount' = [terminalCount EXCEPT ![d] = @ + 1]
    /\ lastEvent' = "Completed"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, ownerGen, beginCommitted, forcedNonOwned,
                    lossPending, selected>>

DuplicateDone(d) ==
    /\ d \in Decisions
    /\ phase[d] = "Terminal"
    /\ lastEvent' = "RejectDuplicateDone"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, phase, ownerGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, lossPending, selected>>

LoseSession ==
    /\ session = "Active"
    /\ session' = "Disconnected"
    /\ confSeen' = FALSE
    /\ lossPending' = TRUE
    /\ lastEvent' = "SessionLost"
    /\ UNCHANGED <<generation, requestState, expected, accepted, phase,
                    ownerGen, slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, selected>>

CleanupLoss ==
    LET live == {d \in Decisions : phase[d] \in LivePhases}
    IN /\ lossPending
       /\ phase' = [d \in Decisions |->
             IF d \in live THEN "Terminal" ELSE phase[d]]
       /\ slotCharged' = [d \in Decisions |-> FALSE]
       /\ terminalCount' = [d \in Decisions |->
             IF d \in live THEN terminalCount[d] + 1 ELSE terminalCount[d]]
       /\ forcedNonOwned' = [d \in Decisions |-> FALSE]
       /\ requestState' = IF requestState = "Idle" THEN "Idle" ELSE "Closed"
       /\ lossPending' = FALSE
       /\ lastEvent' = "SessionCleaned"
       /\ UNCHANGED <<session, generation, confSeen, expected, accepted,
                       ownerGen, beginCommitted, selected>>

LexLE(a, b) ==
    \/ Nice[a] < Nice[b]
    \/ Nice[a] = Nice[b] /\ ClientId[a] <= ClientId[b]

LexMinimal(c) ==
    /\ c \in Clients
    /\ \A other \in Clients : LexLE(c, other)

LexWinner == CHOOSE c \in Clients : LexMinimal(c)

(***************************************************************************
Current-code witness: c0 is visited first. c1 has better niceness but a larger
client id, so the conjunction (lower id AND lower niceness) keeps c0.
***************************************************************************)
ConjWinner ==
    IF ClientId["c1"] < ClientId["c0"] /\ Nice["c1"] < Nice["c0"]
       THEN "c1"
       ELSE "c0"

SelectClient ==
    /\ selected = NoClient
    /\ selected' = IF MutantConjPriority THEN ConjWinner ELSE LexWinner
    /\ lastEvent' = "ClientSelected"
    /\ UNCHANGED <<session, generation, confSeen, requestState, expected,
                    accepted, phase, ownerGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, lossPending>>

Next ==
    \/ Connect
    \/ ActivateLegacy
    \/ ReceiveConf
    \/ SubmitZero
    \/ \E n \in CountChoices : SubmitValid(n)
    \/ \E n \in CountChoices : SubmitOverflow(n)
    \/ \E d \in Decisions : AcceptLocal(d)
    \/ \E d \in Decisions : AcceptRemote(d)
    \/ \E d \in Decisions : BindLocal(d)
    \/ \E d \in Decisions : DeliverLocal(d)
    \/ \E d \in Decisions : CommitBegin(d)
    \/ \E d \in Decisions : FailBegin(d)
    \/ \E d \in Decisions : ArmNonOwned(d)
    \/ \E d \in Decisions : RejectNonOwned(d)
    \/ \E d \in Decisions : WrongDone(d)
    \/ \E d \in Decisions : CompleteDecision(d)
    \/ \E d \in Decisions : DuplicateDone(d)
    \/ LoseSession
    \/ CleanupLoss
    \/ SelectClient

Spec == Init /\ [][Next]_vars
ActivateSession == ActivateLegacy \/ ReceiveConf
FairSpec ==
    /\ Spec
    /\ WF_vars(ActivateSession)
    /\ WF_vars(CleanupLoss)

ProtocolActivation ==
    session = "Active" => (Protocol < 24 \/ confSeen)

BatchBound == expected <= MaxBatch
AcceptedBound == accepted <= expected
AcceptedExact == accepted = Cardinality(AcceptedSet)
CapacityBound == SlotOccupancy <= Capacity

SlotCoherence ==
    \A d \in Decisions : slotCharged[d] <=> phase[d] \in SlotPhases

StartedAfterBegin ==
    \A d \in Decisions : phase[d] = "LocalStarted" => beginCommitted[d]

BeginEvidenceShape ==
    \A d \in Decisions :
        beginCommitted[d] => phase[d] \in {"LocalStarted", "Terminal"}

TerminalShape ==
    \A d \in Decisions :
        /\ (phase[d] = "Terminal") <=> (terminalCount[d] = 1)
        /\ phase[d] = "Terminal" => ~slotCharged[d]

OwnerEvidence ==
    \A d \in Decisions : phase[d] # "Absent" => ownerGen[d] > 0

StartedOwnedWhileLive ==
    \A d \in Decisions :
        phase[d] = "LocalStarted" /\ ~lossPending
        => session = "Active" /\ ownerGen[d] = generation

RejectedCleanup ==
    lastEvent = "RejectNonOwned"
    => \A d \in Decisions :
          forcedNonOwned[d] => phase[d] = "Terminal" /\ ~slotCharged[d]

ZeroNoState ==
    lastEvent = "ZeroNoop"
    => requestState = "Idle" /\ expected = 0 /\ accepted = 0

PriorityMinimal == selected = NoClient \/ LexMinimal(selected)

CoreSafety ==
    /\ TypeOK
    /\ ProtocolActivation
    /\ BatchBound
    /\ AcceptedBound
    /\ AcceptedExact
    /\ CapacityBound
    /\ SlotCoherence
    /\ StartedAfterBegin
    /\ BeginEvidenceShape
    /\ TerminalShape
    /\ OwnerEvidence
    /\ StartedOwnedWhileLive
    /\ RejectedCleanup
    /\ ZeroNoState
    /\ PriorityMinimal

ActivationProgress ==
    [](session = "LoginAttempt" => <> (session = "Active"))

LossCleanupProgress ==
    [](lossPending => <> (~lossPending /\ SlotOccupancy = 0))

=============================================================================
