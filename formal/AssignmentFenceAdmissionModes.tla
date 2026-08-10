---------------- MODULE AssignmentFenceAdmissionModes ----------------
(***************************************************************************
Finite comparison of strict READY-before-UseCS and zero-extra-RTT
pipelined enforcing admission.

Pipelining never means default allow.  An assignment claim that reaches
the fulfillment daemon before PREPARE occupies only a bounded pending
record.  It cannot start a compiler or create an environment side effect
until the matching PREPARE is consumed.  PREPARE and REVOKE share one
ordered scheduler-to-daemon queue.
***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANTS StrictEnforcing, PipelinedEnforcing,
          GeneralScenario, ClaimBeforePrepareScenario, RevokeRaceScenario,
          Mode, Scenario, MaxPending,
          MutantDefaultAllowUnknown,
          MutantSideEffectBeforePrepare,
          MutantUnboundedPending

ASSUME /\ StrictEnforcing # PipelinedEnforcing
       /\ Mode \in {StrictEnforcing, PipelinedEnforcing}
       /\ GeneralScenario # ClaimBeforePrepareScenario
       /\ GeneralScenario # RevokeRaceScenario
       /\ ClaimBeforePrepareScenario # RevokeRaceScenario
       /\ Scenario \in {GeneralScenario,
                         ClaimBeforePrepareScenario,
                         RevokeRaceScenario}
       /\ MaxPending \in Nat \ {0}
       /\ MutantDefaultAllowUnknown \in BOOLEAN
       /\ MutantSideEffectBeforePrepare \in BOOLEAN
       /\ MutantUnboundedPending \in BOOLEAN

ControlKinds == {"PREPARE", "REVOKE"}

ScenarioSequence ==
    CASE Scenario = ClaimBeforePrepareScenario ->
             <<"QueuePrepare", "ExposeUseCS", "ClientClaim",
               "ConsumePrepare", "Start">>
      [] Scenario = RevokeRaceScenario ->
             <<"QueuePrepare", "ExposeUseCS", "ConsumePrepare",
               "QueueRevoke", "ClientClaim", "Start">>
      [] OTHER -> <<>>

ScenarioLength == Len(ScenarioSequence)

Allowed(label) ==
    Scenario = GeneralScenario
    \/ (scenarioStep < ScenarioLength
        /\ ScenarioSequence[scenarioStep + 1] = label)

AdvanceStep ==
    IF Scenario = GeneralScenario THEN scenarioStep ELSE scenarioStep + 1

CapInc(n) == IF n < 2 THEN n + 1 ELSE n

VARIABLES s2f,
          prepareQueuedEver,
          prepared,
          readyQueued,
          readySeen,
          usecsExposed,
          claimArrived,
          pendingCount,
          matchedClaim,
          revoked,
          started,
          sideEffect,
          terminalQueued,
          terminalConsumed,
          released,
          compacted,
          startAfterRelease,
          rejectedUnknown,
          scenarioStep

vars ==
    <<s2f, prepareQueuedEver, prepared, readyQueued, readySeen,
      usecsExposed, claimArrived, pendingCount, matchedClaim, revoked,
      started, sideEffect, terminalQueued, terminalConsumed, released,
      compacted, startAfterRelease, rejectedUnknown, scenarioStep>>

Init ==
    /\ s2f = <<>>
    /\ prepareQueuedEver = FALSE
    /\ prepared = FALSE
    /\ readyQueued = FALSE
    /\ readySeen = FALSE
    /\ usecsExposed = FALSE
    /\ claimArrived = FALSE
    /\ pendingCount = 0
    /\ matchedClaim = FALSE
    /\ revoked = FALSE
    /\ started = FALSE
    /\ sideEffect = FALSE
    /\ terminalQueued = FALSE
    /\ terminalConsumed = FALSE
    /\ released = FALSE
    /\ compacted = FALSE
    /\ startAfterRelease = FALSE
    /\ rejectedUnknown = 0
    /\ scenarioStep = 0

QueuePrepare ==
    /\ Allowed("QueuePrepare")
    /\ ~prepareQueuedEver
    /\ Len(s2f) < 2
    /\ s2f' = Append(s2f, "PREPARE")
    /\ prepareQueuedEver' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepared, readyQueued, readySeen, usecsExposed,
                    claimArrived, pendingCount, matchedClaim, revoked,
                    started, sideEffect, terminalQueued, terminalConsumed,
                    released, compacted, startAfterRelease,
                    rejectedUnknown>>

ExposeUseCS ==
    /\ Allowed("ExposeUseCS")
    /\ ~usecsExposed
    /\ IF Mode = StrictEnforcing THEN readySeen ELSE prepareQueuedEver
    /\ usecsExposed' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, claimArrived, pendingCount, matchedClaim,
                    revoked, started, sideEffect, terminalQueued,
                    terminalConsumed, released, compacted,
                    startAfterRelease, rejectedUnknown>>

ClientClaim ==
    /\ Allowed("ClientClaim")
    /\ usecsExposed
    /\ ~claimArrived
    /\ LET immediatelyMatched == prepared /\ ~revoked
           mayPend == pendingCount < MaxPending \/ MutantUnboundedPending
       IN /\ claimArrived' = TRUE
          /\ matchedClaim' =
                IF immediatelyMatched THEN TRUE ELSE matchedClaim
          /\ pendingCount' =
                IF immediatelyMatched
                THEN pendingCount
                ELSE IF mayPend THEN pendingCount + 1 ELSE pendingCount
          /\ rejectedUnknown' =
                IF immediatelyMatched \/ mayPend
                THEN rejectedUnknown
                ELSE CapInc(rejectedUnknown)
          /\ sideEffect' =
                sideEffect
                \/ (MutantSideEffectBeforePrepare /\ ~prepared)
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, revoked, started,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease>>

UnknownClaim ==
    /\ Allowed("UnknownClaim")
    /\ Mode = PipelinedEnforcing
    /\ usecsExposed
    /\ ~prepared
    /\ pendingCount <= MaxPending
    /\ LET accepted == pendingCount < MaxPending \/ MutantUnboundedPending
       IN /\ pendingCount' =
                IF accepted THEN pendingCount + 1 ELSE pendingCount
          /\ rejectedUnknown' =
                IF accepted THEN rejectedUnknown
                ELSE CapInc(rejectedUnknown)
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived, matchedClaim,
                    revoked, started, sideEffect, terminalQueued,
                    terminalConsumed, released, compacted,
                    startAfterRelease>>

ConsumePrepare ==
    /\ Allowed("ConsumePrepare")
    /\ Len(s2f) > 0
    /\ s2f[1] = "PREPARE"
    /\ LET matchPending == claimArrived /\ pendingCount > 0
       IN /\ s2f' = Tail(s2f)
          /\ prepared' = TRUE
          /\ readyQueued' = TRUE
          /\ matchedClaim' = matchedClaim \/ matchPending
          /\ pendingCount' =
                IF matchPending THEN pendingCount - 1 ELSE pendingCount
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepareQueuedEver, readySeen, usecsExposed,
                    claimArrived, revoked, started, sideEffect,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease, rejectedUnknown>>

ConsumeReady ==
    /\ Allowed("ConsumeReady")
    /\ readyQueued
    /\ ~readySeen
    /\ readySeen' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    usecsExposed, claimArrived, pendingCount,
                    matchedClaim, revoked, started, sideEffect,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease, rejectedUnknown>>

QueueRevoke ==
    /\ Allowed("QueueRevoke")
    /\ prepareQueuedEver
    /\ ~released
    /\ "REVOKE" \notin {s2f[i] : i \in 1..Len(s2f)}
    /\ Len(s2f) < 2
    /\ s2f' = Append(s2f, "REVOKE")
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked, started,
                    sideEffect, terminalQueued, terminalConsumed,
                    released, compacted, startAfterRelease,
                    rejectedUnknown>>

ConsumeRevoke ==
    /\ Allowed("ConsumeRevoke")
    /\ Len(s2f) > 0
    /\ s2f[1] = "REVOKE"
    /\ prepared
    /\ LET fenceWins == ~matchedClaim /\ ~started
       IN /\ s2f' = Tail(s2f)
          /\ revoked' = revoked \/ fenceWins
          /\ terminalQueued' = terminalQueued \/ fenceWins
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, started, sideEffect,
                    terminalConsumed, released, compacted,
                    startAfterRelease, rejectedUnknown>>

Start ==
    /\ Allowed("Start")
    /\ prepared
    /\ matchedClaim
    /\ ~revoked
    /\ ~released
    /\ ~started
    /\ started' = TRUE
    /\ sideEffect' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease, rejectedUnknown>>

ConsumeTerminal ==
    /\ Allowed("ConsumeTerminal")
    /\ terminalQueued
    /\ ~terminalConsumed
    /\ terminalConsumed' = TRUE
    /\ released' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked, started,
                    sideEffect, terminalQueued, compacted,
                    startAfterRelease, rejectedUnknown>>

Compact ==
    /\ Allowed("Compact")
    /\ released
    /\ ~compacted
    /\ compacted' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked, started,
                    sideEffect, terminalQueued, terminalConsumed,
                    released, startAfterRelease, rejectedUnknown>>

DefaultAllowUnknown ==
    /\ Allowed("DefaultAllowUnknown")
    /\ MutantDefaultAllowUnknown
    /\ compacted
    /\ usecsExposed
    /\ ~matchedClaim
    /\ started' = TRUE
    /\ sideEffect' = TRUE
    /\ startAfterRelease' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked,
                    terminalQueued, terminalConsumed, released,
                    compacted, rejectedUnknown>>

Next ==
    \/ QueuePrepare
    \/ ExposeUseCS
    \/ ClientClaim
    \/ UnknownClaim
    \/ ConsumePrepare
    \/ ConsumeReady
    \/ QueueRevoke
    \/ ConsumeRevoke
    \/ Start
    \/ ConsumeTerminal
    \/ Compact
    \/ DefaultAllowUnknown

Spec == Init /\ [][Next]_vars

FairSpec ==
    Spec
    /\ WF_vars(ConsumePrepare)
    /\ WF_vars(ConsumeRevoke)
    /\ WF_vars(ConsumeTerminal)

TypeOK ==
    /\ s2f \in Seq(ControlKinds)
    /\ Len(s2f) <= 2
    /\ prepareQueuedEver \in BOOLEAN
    /\ prepared \in BOOLEAN
    /\ readyQueued \in BOOLEAN
    /\ readySeen \in BOOLEAN
    /\ usecsExposed \in BOOLEAN
    /\ claimArrived \in BOOLEAN
    /\ pendingCount \in 0..(MaxPending + 1)
    /\ matchedClaim \in BOOLEAN
    /\ revoked \in BOOLEAN
    /\ started \in BOOLEAN
    /\ sideEffect \in BOOLEAN
    /\ terminalQueued \in BOOLEAN
    /\ terminalConsumed \in BOOLEAN
    /\ released \in BOOLEAN
    /\ compacted \in BOOLEAN
    /\ startAfterRelease \in BOOLEAN
    /\ rejectedUnknown \in 0..2
    /\ scenarioStep \in 0..ScenarioLength

StrictReadyBeforeUseCS ==
    Mode = StrictEnforcing => (usecsExposed => readySeen)

PrepareQueuedBeforeUseCS == usecsExposed => prepareQueuedEver

MatchedClaimAuthorized ==
    matchedClaim => claimArrived /\ prepared /\ ~revoked

StartAuthorized ==
    started /\ ~startAfterRelease
    => prepared /\ matchedClaim /\ ~revoked /\ ~released

SideEffectAuthorized ==
    sideEffect /\ ~startAfterRelease
    => prepared /\ matchedClaim /\ ~revoked

NoSideEffectBeforePrepare == sideEffect => prepared
PendingBound == pendingCount <= MaxPending
ReleaseAfterTerminalConsume == released => terminalConsumed
RevokedExcludesClaim == revoked => ~matchedClaim /\ ~started
TerminalRequiresFence == terminalQueued => revoked
NoStartAfterRelease == ~startAfterRelease

SafetyInvariant ==
    /\ TypeOK
    /\ StrictReadyBeforeUseCS
    /\ PrepareQueuedBeforeUseCS
    /\ MatchedClaimAuthorized
    /\ StartAuthorized
    /\ SideEffectAuthorized
    /\ NoSideEffectBeforePrepare
    /\ PendingBound
    /\ ReleaseAfterTerminalConsume
    /\ RevokedExcludesClaim
    /\ TerminalRequiresFence
    /\ NoStartAfterRelease

PendingEventuallyResolved ==
    []((claimArrived /\ pendingCount > 0)
       => <>(matchedClaim \/ revoked \/ released))

ScenarioComplete == scenarioStep = ScenarioLength

NoClaimBeforePrepareWitness ==
    ~(Scenario = ClaimBeforePrepareScenario
      /\ ScenarioComplete
      /\ started)

NoRevokeRaceWitness ==
    ~(Scenario = RevokeRaceScenario
      /\ ScenarioComplete
      /\ started)

=============================================================================
