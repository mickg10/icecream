------------------------------ MODULE G4Lifecycle ------------------------------
(***************************************************************************
Small parameterized model for the issue-4 daemon/session and count>1 batch
lifecycle.  It is a counterexample-search model, not a product implementation
or a topology-general proof.

External arrival is separated from daemon-owned handling: ConfArrives has no
fairness assumption; HandleConf is weakly fair once the frame has arrived.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Protocol,
          ClientCount,
          DecisionCount,
          MaxBatch,
          Capacity,
          MaxGeneration,
          MaxRequestGeneration,
          MutantLegacyNeedsConf,
          MutantStartBeforeBegin,
          MutantPartialCleanup,
          MutantUnboundedBatch,
          MutantIdFirstPriority,
          MutantAllowSecondBound,
          MutantStaleDecision,
          MutantCompleteWhileDisconnected,
          MutantCapacityLE,
          MutantDuplicateTerminal

ASSUME /\ Protocol \in Nat
       /\ ClientCount \in 1..3
       /\ DecisionCount \in 1..3
       /\ MaxBatch \in 1..DecisionCount
       /\ Capacity \in 1..2
       /\ MaxGeneration \in 2..3
       /\ MaxRequestGeneration \in 1..3
       /\ MutantLegacyNeedsConf \in BOOLEAN
       /\ MutantStartBeforeBegin \in BOOLEAN
       /\ MutantPartialCleanup \in BOOLEAN
       /\ MutantUnboundedBatch \in BOOLEAN
       /\ MutantIdFirstPriority \in BOOLEAN
       /\ MutantAllowSecondBound \in BOOLEAN
       /\ MutantStaleDecision \in BOOLEAN
       /\ MutantCompleteWhileDisconnected \in BOOLEAN
       /\ MutantCapacityLE \in BOOLEAN
       /\ MutantDuplicateTerminal \in BOOLEAN

Clients == 1..ClientCount
Decisions == 1..DecisionCount
Pairs == Clients \X Decisions
NoClient == 0
NoDecision == 0

ClientId(c) == c
Nice(c) == IF c = 1 THEN 10 ELSE 0

Sessions == {"Disconnected", "LoginAttempt", "Active"}
ActivationModes == {"None", "Legacy", "Conf"}
RequestStates == {"Idle", "Waiting", "Closed"}
DecisionKinds == {"None", "Remote", "Local", "NoCS"}
DecisionPhases == {
    "Absent",
    "RemoteDelivered",
    "LocalWaiting",
    "LocalBound",
    "LocalDelivered",
    "LocalStarted",
    "Terminal"
}
LivePhases == DecisionPhases \ {"Absent", "Terminal"}
SlotPhases == {"LocalDelivered", "LocalStarted"}
DonePhases == {"RemoteDelivered", "LocalStarted"}
AcceptedPhases == DecisionPhases \ {"Absent"}

Events == {
    "Init", "Connect", "ConfArrived", "LegacyActivated", "ConfActivated",
    "ZeroNoop", "BatchAccepted", "BatchOverflowRejected",
    "LocalAccepted", "RemoteAccepted", "NoCSAccepted",
    "StaleDecisionRejected", "LocalBound", "LocalDelivered",
    "BeginCommitted", "BeginNoCommitFailure", "NonOwnedObserved",
    "NonOwnedRejected", "WrongDoneRejected", "Completed",
    "DuplicateDoneRejected", "SessionLost", "SessionCleaned",
    "RequestClosed", "ClientReset"
}

GenerationSet == 0..MaxGeneration
RequestGenerationSet == 0..MaxRequestGeneration
TokenUniverse == GenerationSet \X Clients \X RequestGenerationSet \X Decisions

VARIABLES session,
          generation,
          confArrived,
          activatedBy,
          lossPending,
          requestState,
          requestGeneration,
          expected,
          accepted,
          kind,
          phase,
          ownerGen,
          ownerRequestGen,
          slotCharged,
          beginCommitted,
          terminalCount,
          forcedNonOwned,
          completedTokens,
          rejectedStaleTokens,
          selectedClient,
          lastEligible,
          lastEvent,
          lastClient,
          lastDecision

vars == <<session, generation, confArrived, activatedBy, lossPending,
          requestState, requestGeneration, expected, accepted, kind, phase,
          ownerGen, ownerRequestGen, slotCharged, beginCommitted,
          terminalCount, forcedNonOwned, completedTokens,
          rejectedStaleTokens, selectedClient, lastEligible,
          lastEvent, lastClient, lastDecision>>

CurrentToken(c, d) ==
    <<ownerGen[c][d], c, ownerRequestGen[c][d], d>>

LivePairs ==
    {p \in Pairs : phase[p[1]][p[2]] \in LivePhases}

BoundPairs ==
    {p \in Pairs : phase[p[1]][p[2]] = "LocalBound"}

ChargedPairs ==
    {p \in Pairs : slotCharged[p[1]][p[2]]}

SlotOccupancy == Cardinality(ChargedPairs)

AcceptedSet(c) ==
    {d \in Decisions : phase[c][d] \in AcceptedPhases}

EligiblePairs ==
    {p \in Pairs :
        LET c == p[1]
            d == p[2]
        IN /\ session = "Active"
           /\ ~lossPending
           /\ requestState[c] = "Waiting"
           /\ phase[c][d] = "LocalWaiting"
           /\ ownerGen[c][d] = generation
           /\ ownerRequestGen[c][d] = requestGeneration[c]}

EligibleClients ==
    {c \in Clients : \E d \in Decisions : <<c, d>> \in EligiblePairs}

LexLE(a, b) ==
    \/ Nice(a) < Nice(b)
    \/ Nice(a) = Nice(b) /\ ClientId(a) <= ClientId(b)

LexMinimalIn(c, es) ==
    /\ c \in es
    /\ \A other \in es : LexLE(c, other)

IdMinimalIn(c, es) ==
    /\ c \in es
    /\ \A other \in es : ClientId(c) <= ClientId(other)

PolicySelects(c) ==
    IF MutantIdFirstPriority
       THEN IdMinimalIn(c, EligibleClients)
       ELSE LexMinimalIn(c, EligibleClients)

TypeOK ==
    /\ session \in Sessions
    /\ generation \in GenerationSet
    /\ confArrived \in BOOLEAN
    /\ activatedBy \in ActivationModes
    /\ lossPending \in BOOLEAN
    /\ requestState \in [Clients -> RequestStates]
    /\ requestGeneration \in [Clients -> RequestGenerationSet]
    /\ expected \in [Clients -> 0..(MaxBatch + 1)]
    /\ accepted \in [Clients -> 0..DecisionCount]
    /\ kind \in [Clients -> [Decisions -> DecisionKinds]]
    /\ phase \in [Clients -> [Decisions -> DecisionPhases]]
    /\ ownerGen \in [Clients -> [Decisions -> GenerationSet]]
    /\ ownerRequestGen \in
         [Clients -> [Decisions -> RequestGenerationSet]]
    /\ slotCharged \in [Clients -> [Decisions -> BOOLEAN]]
    /\ beginCommitted \in [Clients -> [Decisions -> BOOLEAN]]
    /\ terminalCount \in [Clients -> [Decisions -> 0..2]]
    /\ forcedNonOwned \in [Clients -> [Decisions -> BOOLEAN]]
    /\ completedTokens \subseteq TokenUniverse
    /\ rejectedStaleTokens \subseteq TokenUniverse
    /\ selectedClient \in Clients \cup {NoClient}
    /\ lastEligible \subseteq Clients
    /\ lastEvent \in Events
    /\ lastClient \in Clients \cup {NoClient}
    /\ lastDecision \in Decisions \cup {NoDecision}

Init ==
    /\ session = "Disconnected"
    /\ generation = 0
    /\ confArrived = FALSE
    /\ activatedBy = "None"
    /\ lossPending = FALSE
    /\ requestState = [c \in Clients |-> "Idle"]
    /\ requestGeneration = [c \in Clients |-> 0]
    /\ expected = [c \in Clients |-> 0]
    /\ accepted = [c \in Clients |-> 0]
    /\ kind = [c \in Clients |-> [d \in Decisions |-> "None"]]
    /\ phase = [c \in Clients |-> [d \in Decisions |-> "Absent"]]
    /\ ownerGen = [c \in Clients |-> [d \in Decisions |-> 0]]
    /\ ownerRequestGen = [c \in Clients |-> [d \in Decisions |-> 0]]
    /\ slotCharged = [c \in Clients |-> [d \in Decisions |-> FALSE]]
    /\ beginCommitted = [c \in Clients |-> [d \in Decisions |-> FALSE]]
    /\ terminalCount = [c \in Clients |-> [d \in Decisions |-> 0]]
    /\ forcedNonOwned = [c \in Clients |-> [d \in Decisions |-> FALSE]]
    /\ completedTokens = {}
    /\ rejectedStaleTokens = {}
    /\ selectedClient = NoClient
    /\ lastEligible = {}
    /\ lastEvent = "Init"
    /\ lastClient = NoClient
    /\ lastDecision = NoDecision

Connect ==
    /\ session = "Disconnected"
    /\ ~lossPending
    /\ generation < MaxGeneration
    /\ \A c \in Clients : requestState[c] = "Idle"
    /\ session' = "LoginAttempt"
    /\ confArrived' = FALSE
    /\ activatedBy' = "None"
    /\ lastEvent' = "Connect"
    /\ lastClient' = NoClient
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<generation, lossPending, requestState,
                    requestGeneration, expected, accepted, kind, phase,
                    ownerGen, ownerRequestGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

ActivateLegacy ==
    /\ session = "LoginAttempt"
    /\ Protocol < 24
    /\ ~MutantLegacyNeedsConf
    /\ generation < MaxGeneration
    /\ session' = "Active"
    /\ generation' = generation + 1
    /\ activatedBy' = "Legacy"
    /\ lastEvent' = "LegacyActivated"
    /\ lastClient' = NoClient
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<confArrived, lossPending, requestState,
                    requestGeneration, expected, accepted, kind, phase,
                    ownerGen, ownerRequestGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

ConfArrives ==
    /\ session = "LoginAttempt"
    /\ Protocol >= 24
    /\ ~confArrived
    /\ confArrived' = TRUE
    /\ lastEvent' = "ConfArrived"
    /\ lastClient' = NoClient
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<session, generation, activatedBy, lossPending,
                    requestState, requestGeneration, expected, accepted,
                    kind, phase, ownerGen, ownerRequestGen, slotCharged,
                    beginCommitted, terminalCount, forcedNonOwned,
                    completedTokens, rejectedStaleTokens, selectedClient,
                    lastEligible>>

HandleConf ==
    /\ session = "LoginAttempt"
    /\ Protocol >= 24
    /\ confArrived
    /\ generation < MaxGeneration
    /\ session' = "Active"
    /\ generation' = generation + 1
    /\ confArrived' = FALSE
    /\ activatedBy' = "Conf"
    /\ lastEvent' = "ConfActivated"
    /\ lastClient' = NoClient
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<lossPending, requestState, requestGeneration,
                    expected, accepted, kind, phase, ownerGen,
                    ownerRequestGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

SubmitZero(c) ==
    /\ c \in Clients
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Idle"
    /\ lastEvent' = "ZeroNoop"
    /\ lastClient' = c
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration,
                    expected, accepted, kind, phase, ownerGen,
                    ownerRequestGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

SubmitValid(c, n) ==
    /\ c \in Clients
    /\ n \in 1..MaxBatch
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Idle"
    /\ requestGeneration[c] < MaxRequestGeneration
    /\ \A d \in Decisions : phase[c][d] = "Absent"
    /\ requestState' = [requestState EXCEPT ![c] = "Waiting"]
    /\ requestGeneration' =
         [requestGeneration EXCEPT ![c] = @ + 1]
    /\ expected' = [expected EXCEPT ![c] = n]
    /\ accepted' = [accepted EXCEPT ![c] = 0]
    /\ lastEvent' = "BatchAccepted"
    /\ lastClient' = c
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, kind, phase, ownerGen, ownerRequestGen,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, completedTokens, rejectedStaleTokens,
                    selectedClient, lastEligible>>

SubmitOverflow(c) ==
    /\ c \in Clients
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Idle"
    /\ requestGeneration[c] < MaxRequestGeneration
    /\ \A d \in Decisions : phase[c][d] = "Absent"
    /\ IF MutantUnboundedBatch
          THEN /\ requestState' = [requestState EXCEPT ![c] = "Waiting"]
               /\ requestGeneration' =
                    [requestGeneration EXCEPT ![c] = @ + 1]
               /\ expected' = [expected EXCEPT ![c] = MaxBatch + 1]
               /\ lastEvent' = "BatchAccepted"
          ELSE /\ requestState' = [requestState EXCEPT ![c] = "Closed"]
               /\ requestGeneration' = requestGeneration
               /\ expected' = expected
               /\ lastEvent' = "BatchOverflowRejected"
    /\ lastClient' = c
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, accepted, kind, phase, ownerGen,
                    ownerRequestGen, slotCharged, beginCommitted,
                    terminalCount, forcedNonOwned, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

AcceptLocal(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Waiting"
    /\ accepted[c] < expected[c]
    /\ d <= expected[c]
    /\ phase[c][d] = "Absent"
    /\ phase' = [phase EXCEPT ![c][d] = "LocalWaiting"]
    /\ kind' = [kind EXCEPT ![c][d] = "Local"]
    /\ ownerGen' = [ownerGen EXCEPT ![c][d] = generation]
    /\ ownerRequestGen' =
         [ownerRequestGen EXCEPT ![c][d] = requestGeneration[c]]
    /\ accepted' = [accepted EXCEPT ![c] = @ + 1]
    /\ lastEvent' = "LocalAccepted"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, completedTokens, rejectedStaleTokens,
                    selectedClient, lastEligible>>

AcceptRemote(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Waiting"
    /\ accepted[c] < expected[c]
    /\ d <= expected[c]
    /\ phase[c][d] = "Absent"
    /\ phase' = [phase EXCEPT ![c][d] = "RemoteDelivered"]
    /\ kind' = [kind EXCEPT ![c][d] = "Remote"]
    /\ ownerGen' = [ownerGen EXCEPT ![c][d] = generation]
    /\ ownerRequestGen' =
         [ownerRequestGen EXCEPT ![c][d] = requestGeneration[c]]
    /\ accepted' = [accepted EXCEPT ![c] = @ + 1]
    /\ lastEvent' = "RemoteAccepted"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, completedTokens, rejectedStaleTokens,
                    selectedClient, lastEligible>>

AcceptNoCS(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Waiting"
    /\ accepted[c] < expected[c]
    /\ d <= expected[c]
    /\ phase[c][d] = "Absent"
    /\ phase' = [phase EXCEPT ![c][d] = "Terminal"]
    /\ kind' = [kind EXCEPT ![c][d] = "NoCS"]
    /\ ownerGen' = [ownerGen EXCEPT ![c][d] = generation]
    /\ ownerRequestGen' =
         [ownerRequestGen EXCEPT ![c][d] = requestGeneration[c]]
    /\ terminalCount' = [terminalCount EXCEPT ![c][d] = 1]
    /\ accepted' = [accepted EXCEPT ![c] = @ + 1]
    /\ completedTokens' = completedTokens \cup
         {<<generation, c, requestGeneration[c], d>>}
    /\ lastEvent' = "NoCSAccepted"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    slotCharged, beginCommitted, forcedNonOwned,
                    rejectedStaleTokens, selectedClient, lastEligible>>

ReceiveStaleLocal(c, d, sg, rg) ==
    LET token == <<sg, c, rg, d>>
    IN /\ c \in Clients
       /\ d \in Decisions
       /\ sg \in GenerationSet
       /\ rg \in RequestGenerationSet
       /\ session = "Active"
       /\ ~lossPending
       /\ requestState[c] = "Waiting"
       /\ phase[c][d] = "Absent"
       /\ accepted[c] < expected[c]
       /\ d <= expected[c]
       /\ \/ sg # generation
          \/ rg # requestGeneration[c]
       /\ token \notin rejectedStaleTokens
       /\ IF MutantStaleDecision
             THEN /\ phase' = [phase EXCEPT ![c][d] = "LocalWaiting"]
                  /\ kind' = [kind EXCEPT ![c][d] = "Local"]
                  /\ ownerGen' = [ownerGen EXCEPT ![c][d] = sg]
                  /\ ownerRequestGen' =
                       [ownerRequestGen EXCEPT ![c][d] = rg]
                  /\ accepted' = [accepted EXCEPT ![c] = @ + 1]
             ELSE /\ phase' = phase
                  /\ kind' = kind
                  /\ ownerGen' = ownerGen
                  /\ ownerRequestGen' = ownerRequestGen
                  /\ accepted' = accepted
       /\ rejectedStaleTokens' = rejectedStaleTokens \cup {token}
       /\ lastEvent' = "StaleDecisionRejected"
       /\ lastClient' = c
       /\ lastDecision' = d
       /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                       lossPending, requestState, requestGeneration, expected,
                       slotCharged, beginCommitted, terminalCount,
                       forcedNonOwned, completedTokens, selectedClient,
                       lastEligible>>

BindLocal(c, d) ==
    /\ <<c, d>> \in EligiblePairs
    /\ PolicySelects(c)
    /\ \/ MutantAllowSecondBound
       \/ BoundPairs = {}
    /\ phase' = [phase EXCEPT ![c][d] = "LocalBound"]
    /\ selectedClient' = c
    /\ lastEligible' = EligibleClients
    /\ lastEvent' = "LocalBound"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, ownerGen, ownerRequestGen, slotCharged,
                    beginCommitted, terminalCount, forcedNonOwned,
                    completedTokens, rejectedStaleTokens>>

DeliverLocal(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Waiting"
    /\ phase[c][d] = "LocalBound"
    /\ ownerGen[c][d] = generation
    /\ ownerRequestGen[c][d] = requestGeneration[c]
    /\ IF MutantCapacityLE
          THEN SlotOccupancy <= Capacity
          ELSE SlotOccupancy < Capacity
    /\ phase' = [phase EXCEPT ![c][d] = "LocalDelivered"]
    /\ slotCharged' = [slotCharged EXCEPT ![c][d] = TRUE]
    /\ lastEvent' = "LocalDelivered"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, ownerGen, ownerRequestGen,
                    beginCommitted, terminalCount, forcedNonOwned,
                    completedTokens, rejectedStaleTokens, selectedClient,
                    lastEligible>>

CommitBegin(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Waiting"
    /\ phase[c][d] = "LocalDelivered"
    /\ ownerGen[c][d] = generation
    /\ ownerRequestGen[c][d] = requestGeneration[c]
    /\ ~forcedNonOwned[c][d]
    /\ phase' = [phase EXCEPT ![c][d] = "LocalStarted"]
    /\ beginCommitted' = [beginCommitted EXCEPT ![c][d] = TRUE]
    /\ lastEvent' = "BeginCommitted"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, ownerGen, ownerRequestGen, slotCharged,
                    terminalCount, forcedNonOwned, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

FailBegin(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ ~lossPending
    /\ requestState[c] = "Waiting"
    /\ phase[c][d] = "LocalDelivered"
    /\ ownerGen[c][d] = generation
    /\ ownerRequestGen[c][d] = requestGeneration[c]
    /\ ~forcedNonOwned[c][d]
    /\ phase' = [phase EXCEPT
          ![c][d] = IF MutantStartBeforeBegin THEN "LocalStarted" ELSE @]
    /\ session' = "Disconnected"
    /\ confArrived' = FALSE
    /\ activatedBy' = "None"
    /\ lossPending' = TRUE
    /\ lastEvent' = "BeginNoCommitFailure"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<generation, requestState, requestGeneration, expected,
                    accepted, kind, ownerGen, ownerRequestGen, slotCharged,
                    beginCommitted, terminalCount, forcedNonOwned,
                    completedTokens, rejectedStaleTokens, selectedClient,
                    lastEligible>>

ObserveNonOwned(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ phase[c][d] = "LocalDelivered"
    /\ ~forcedNonOwned[c][d]
    /\ forcedNonOwned' = [forcedNonOwned EXCEPT ![c][d] = TRUE]
    /\ lastEvent' = "NonOwnedObserved"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, phase, ownerGen, ownerRequestGen,
                    slotCharged, beginCommitted, terminalCount,
                    completedTokens, rejectedStaleTokens, selectedClient,
                    lastEligible>>

RejectNonOwned(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ session = "Active"
    /\ phase[c][d] = "LocalDelivered"
    /\ forcedNonOwned[c][d]
    /\ session' = "Disconnected"
    /\ confArrived' = FALSE
    /\ activatedBy' = "None"
    /\ lossPending' = TRUE
    /\ lastEvent' = "NonOwnedRejected"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<generation, requestState, requestGeneration, expected,
                    accepted, kind, phase, ownerGen, ownerRequestGen,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, completedTokens, rejectedStaleTokens,
                    selectedClient, lastEligible>>

WrongDone(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ phase[c][d] \in {"LocalWaiting", "LocalBound", "LocalDelivered"}
    /\ lastEvent' = "WrongDoneRejected"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, phase, ownerGen, ownerRequestGen,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, completedTokens, rejectedStaleTokens,
                    selectedClient, lastEligible>>

CompleteDecision(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ phase[c][d] \in DonePhases
    /\ \/ MutantCompleteWhileDisconnected
       \/ /\ session = "Active"
          /\ ~lossPending
          /\ requestState[c] = "Waiting"
          /\ ownerGen[c][d] = generation
          /\ ownerRequestGen[c][d] = requestGeneration[c]
    /\ phase' = [phase EXCEPT ![c][d] = "Terminal"]
    /\ slotCharged' = [slotCharged EXCEPT ![c][d] = FALSE]
    /\ terminalCount' = [terminalCount EXCEPT ![c][d] = @ + 1]
    /\ completedTokens' = completedTokens \cup {CurrentToken(c, d)}
    /\ lastEvent' = "Completed"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, ownerGen, ownerRequestGen,
                    beginCommitted, forcedNonOwned, rejectedStaleTokens,
                    selectedClient, lastEligible>>

DuplicateDone(c, d) ==
    /\ c \in Clients
    /\ d \in Decisions
    /\ phase[c][d] = "Terminal"
    /\ terminalCount' = [terminalCount EXCEPT
          ![c][d] = IF MutantDuplicateTerminal THEN @ + 1 ELSE @]
    /\ lastEvent' = "DuplicateDoneRejected"
    /\ lastClient' = c
    /\ lastDecision' = d
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestState, requestGeneration, expected,
                    accepted, kind, phase, ownerGen, ownerRequestGen,
                    slotCharged, beginCommitted, forcedNonOwned,
                    completedTokens, rejectedStaleTokens, selectedClient,
                    lastEligible>>

LoseSession ==
    /\ session = "Active"
    /\ ~lossPending
    /\ session' = "Disconnected"
    /\ confArrived' = FALSE
    /\ activatedBy' = "None"
    /\ lossPending' = TRUE
    /\ lastEvent' = "SessionLost"
    /\ lastClient' = NoClient
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<generation, requestState, requestGeneration, expected,
                    accepted, kind, phase, ownerGen, ownerRequestGen,
                    slotCharged, beginCommitted, terminalCount,
                    forcedNonOwned, completedTokens, rejectedStaleTokens,
                    selectedClient, lastEligible>>

CleanupLoss ==
    LET live == LivePairs
        forced == {p \in live : forcedNonOwned[p[1]][p[2]]}
        clean == IF MutantPartialCleanup /\ forced # {} THEN forced ELSE live
    IN /\ lossPending
       /\ session = "Disconnected"
       /\ phase' = [c \in Clients |-> [d \in Decisions |->
             IF <<c, d>> \in clean THEN "Terminal" ELSE phase[c][d]]]
       /\ slotCharged' = [c \in Clients |-> [d \in Decisions |->
             IF <<c, d>> \in clean THEN FALSE ELSE slotCharged[c][d]]]
       /\ terminalCount' = [c \in Clients |-> [d \in Decisions |->
             IF <<c, d>> \in clean THEN terminalCount[c][d] + 1
             ELSE terminalCount[c][d]]]
       /\ forcedNonOwned' =
            [c \in Clients |-> [d \in Decisions |-> FALSE]]
       /\ requestState' = [c \in Clients |->
             IF requestState[c] = "Idle" THEN "Idle" ELSE "Closed"]
       /\ completedTokens' = completedTokens \cup
            {CurrentToken(p[1], p[2]) : p \in clean}
       /\ lossPending' = FALSE
       /\ lastEvent' = "SessionCleaned"
       /\ lastClient' = NoClient
       /\ lastDecision' = NoDecision
       /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                       requestGeneration, expected, accepted, kind,
                       ownerGen, ownerRequestGen, beginCommitted,
                       rejectedStaleTokens, selectedClient, lastEligible>>

CloseSettledRequest(c) ==
    /\ c \in Clients
    /\ requestState[c] = "Waiting"
    /\ accepted[c] = expected[c]
    /\ \A d \in 1..expected[c] : phase[c][d] = "Terminal"
    /\ requestState' = [requestState EXCEPT ![c] = "Closed"]
    /\ lastEvent' = "RequestClosed"
    /\ lastClient' = c
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestGeneration, expected, accepted,
                    kind, phase, ownerGen, ownerRequestGen, slotCharged,
                    beginCommitted, terminalCount, forcedNonOwned,
                    completedTokens, rejectedStaleTokens, selectedClient,
                    lastEligible>>

ResetClient(c) ==
    /\ c \in Clients
    /\ requestState[c] = "Closed"
    /\ \A d \in Decisions :
         /\ phase[c][d] \in {"Absent", "Terminal"}
         /\ ~slotCharged[c][d]
    /\ requestState' = [requestState EXCEPT ![c] = "Idle"]
    /\ expected' = [expected EXCEPT ![c] = 0]
    /\ accepted' = [accepted EXCEPT ![c] = 0]
    /\ kind' = [kind EXCEPT ![c] = [d \in Decisions |-> "None"]]
    /\ phase' = [phase EXCEPT ![c] = [d \in Decisions |-> "Absent"]]
    /\ ownerGen' = [ownerGen EXCEPT ![c] = [d \in Decisions |-> 0]]
    /\ ownerRequestGen' =
         [ownerRequestGen EXCEPT ![c] = [d \in Decisions |-> 0]]
    /\ slotCharged' =
         [slotCharged EXCEPT ![c] = [d \in Decisions |-> FALSE]]
    /\ beginCommitted' =
         [beginCommitted EXCEPT ![c] = [d \in Decisions |-> FALSE]]
    /\ terminalCount' =
         [terminalCount EXCEPT ![c] = [d \in Decisions |-> 0]]
    /\ forcedNonOwned' =
         [forcedNonOwned EXCEPT ![c] = [d \in Decisions |-> FALSE]]
    /\ lastEvent' = "ClientReset"
    /\ lastClient' = c
    /\ lastDecision' = NoDecision
    /\ UNCHANGED <<session, generation, confArrived, activatedBy,
                    lossPending, requestGeneration, completedTokens,
                    rejectedStaleTokens, selectedClient, lastEligible>>

Next ==
    \/ Connect
    \/ ActivateLegacy
    \/ ConfArrives
    \/ HandleConf
    \/ \E c \in Clients : SubmitZero(c)
    \/ \E c \in Clients, n \in 1..MaxBatch : SubmitValid(c, n)
    \/ \E c \in Clients : SubmitOverflow(c)
    \/ \E c \in Clients, d \in Decisions : AcceptLocal(c, d)
    \/ \E c \in Clients, d \in Decisions : AcceptRemote(c, d)
    \/ \E c \in Clients, d \in Decisions : AcceptNoCS(c, d)
    \/ \E c \in Clients, d \in Decisions,
          sg \in GenerationSet, rg \in RequestGenerationSet :
         ReceiveStaleLocal(c, d, sg, rg)
    \/ \E c \in Clients, d \in Decisions : BindLocal(c, d)
    \/ \E c \in Clients, d \in Decisions : DeliverLocal(c, d)
    \/ \E c \in Clients, d \in Decisions : CommitBegin(c, d)
    \/ \E c \in Clients, d \in Decisions : FailBegin(c, d)
    \/ \E c \in Clients, d \in Decisions : ObserveNonOwned(c, d)
    \/ \E c \in Clients, d \in Decisions : RejectNonOwned(c, d)
    \/ \E c \in Clients, d \in Decisions : WrongDone(c, d)
    \/ \E c \in Clients, d \in Decisions : CompleteDecision(c, d)
    \/ \E c \in Clients, d \in Decisions : DuplicateDone(c, d)
    \/ LoseSession
    \/ CleanupLoss
    \/ \E c \in Clients : CloseSettledRequest(c)
    \/ \E c \in Clients : ResetClient(c)

SafetySpec == Init /\ [][Next]_vars

FairSpec ==
    /\ SafetySpec
    /\ WF_vars(ActivateLegacy)
    /\ WF_vars(HandleConf)
    /\ WF_vars(CleanupLoss)

ProtocolActivation ==
    session = "Active"
    => IF Protocol < 24 THEN activatedBy = "Legacy" ELSE activatedBy = "Conf"

BatchBound ==
    \A c \in Clients : expected[c] <= MaxBatch

AcceptedBound ==
    \A c \in Clients : accepted[c] <= expected[c]

AcceptedExact ==
    \A c \in Clients : accepted[c] = Cardinality(AcceptedSet(c))

CapacityBound == SlotOccupancy <= Capacity

OneBoundLocal == Cardinality(BoundPairs) <= 1

SlotCoherence ==
    \A p \in Pairs :
      LET c == p[1]
          d == p[2]
      IN slotCharged[c][d] <=> phase[c][d] \in SlotPhases

StartedAfterBegin ==
    \A p \in Pairs :
      LET c == p[1]
          d == p[2]
      IN phase[c][d] = "LocalStarted" => beginCommitted[c][d]

BeginEvidenceShape ==
    \A p \in Pairs :
      LET c == p[1]
          d == p[2]
      IN beginCommitted[c][d]
         => phase[c][d] \in {"LocalStarted", "Terminal"}

TerminalShape ==
    \A p \in Pairs :
      LET c == p[1]
          d == p[2]
      IN /\ (phase[c][d] = "Terminal") <=> (terminalCount[c][d] >= 1)
         /\ phase[c][d] = "Terminal" => ~slotCharged[c][d]

TerminalUniqueness ==
    \A p \in Pairs : terminalCount[p[1]][p[2]] <= 1

OwnerEvidence ==
    \A p \in Pairs :
      LET c == p[1]
          d == p[2]
      IN phase[c][d] # "Absent"
         => /\ ownerGen[c][d] > 0
            /\ ownerRequestGen[c][d] > 0

LiveOwnedCurrent ==
    \A p \in LivePairs :
      LET c == p[1]
          d == p[2]
      IN session = "Active" /\ ~lossPending
         => /\ ownerGen[c][d] = generation
            /\ ownerRequestGen[c][d] = requestGeneration[c]

CompletedTokenNotLive ==
    \A p \in LivePairs : CurrentToken(p[1], p[2]) \notin completedTokens

RequestIdleShape ==
    \A c \in Clients :
      requestState[c] = "Idle"
      => /\ expected[c] = 0
         /\ accepted[c] = 0
         /\ \A d \in Decisions : phase[c][d] = "Absent"

RequestClosedShape ==
    \A c \in Clients :
      requestState[c] = "Closed"
      => /\ \A d \in Decisions : phase[c][d] \in {"Absent", "Terminal"}
         /\ \A d \in Decisions : ~slotCharged[c][d]

DisconnectedClean ==
    session = "Disconnected" /\ ~lossPending
    => /\ LivePairs = {}
       /\ SlotOccupancy = 0

PriorityMinimal ==
    selectedClient = NoClient
    \/ /\ selectedClient \in lastEligible
       /\ \A other \in lastEligible : LexLE(selectedClient, other)

ZeroNoState ==
    lastEvent = "ZeroNoop"
    => /\ lastClient \in Clients
       /\ requestState[lastClient] = "Idle"
       /\ expected[lastClient] = 0
       /\ accepted[lastClient] = 0

CompletionAuthority ==
    lastEvent = "Completed"
    => /\ session = "Active"
       /\ ~lossPending
       /\ lastClient \in Clients
       /\ lastDecision \in Decisions
       /\ ownerGen[lastClient][lastDecision] = generation
       /\ ownerRequestGen[lastClient][lastDecision] =
            requestGeneration[lastClient]

CoreSafety ==
    /\ TypeOK
    /\ ProtocolActivation
    /\ BatchBound
    /\ AcceptedBound
    /\ AcceptedExact
    /\ CapacityBound
    /\ OneBoundLocal
    /\ SlotCoherence
    /\ StartedAfterBegin
    /\ BeginEvidenceShape
    /\ TerminalShape
    /\ TerminalUniqueness
    /\ OwnerEvidence
    /\ LiveOwnedCurrent
    /\ CompletedTokenNotLive
    /\ RequestIdleShape
    /\ RequestClosedShape
    /\ DisconnectedClean
    /\ PriorityMinimal
    /\ ZeroNoState
    /\ CompletionAuthority

LegacyActivationProgress ==
    [](Protocol < 24 /\ session = "LoginAttempt" => <> (session = "Active"))

ConfActivationProgress ==
    [](Protocol >= 24 /\ session = "LoginAttempt" /\ confArrived
       => <> (session = "Active"))

LossCleanupProgress ==
    [](lossPending => <> (~lossPending /\ LivePairs = {} /\ SlotOccupancy = 0))

NeverSecondGeneration == generation < 2

=============================================================================
