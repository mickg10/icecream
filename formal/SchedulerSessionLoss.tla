----------------------- MODULE SchedulerSessionLoss -----------------------
(***************************************************************************
Protocol-neutral scheduler-session loss state machine for iceccd G4.

The load-bearing distinction is semantic, not pointer-based:

  DISCONNECTED -> LOGIN_ATTEMPT -> ACTIVE only when ConfCS is consumed.

A channel failure before ConfCS is an attempt failure: schedulerless local
work survives and no established-session cleanup is charged.  A failure after
ConfCS creates one generation-scoped loss token.  Product-controlled cleanup
settles that token before clearing old-session clients, and no later event from
the same poll turn may execute.

The model deliberately excludes compiler termination and reconnect fairness.
Only FinishLoss is weakly fair in FixedFairSpec.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS MaxGeneration,
          InitialGeneration,
          CountCap,
          MutantActivateBeforeConf,
          MutantAllowPostLossWork,
          MutantDuplicateCleanup,
          MutantNumericSentinel,
          MutantShutdownAsLoss,
          MutantWrapGeneration

ASSUME /\ MaxGeneration \in Nat \ {0}
       /\ InitialGeneration \in 0..MaxGeneration
       /\ CountCap \in Nat \ {0}
       /\ MutantActivateBeforeConf \in BOOLEAN
       /\ MutantAllowPostLossWork \in BOOLEAN
       /\ MutantDuplicateCleanup \in BOOLEAN
       /\ MutantNumericSentinel \in BOOLEAN
       /\ MutantShutdownAsLoss \in BOOLEAN
       /\ MutantWrapGeneration \in BOOLEAN

Phases == {"Disconnected", "LoginAttempt", "Active", "LossPending", "Shutdown"}
Generations == 0..MaxGeneration

CapInc(n) == IF n < CountCap THEN n + 1 ELSE n

NextGeneration(g) == IF g = MaxGeneration THEN 0 ELSE g + 1

VARIABLES phase,
          channelUp,
          confReceived,
          generationValid,
          generation,
          settledValid,
          settledGeneration,
          cleanupAttempts,
          attemptFailureCleanups,
          unexpectedShutdownCleanups,
          lostGenerations,
          cleanedGenerations,
          retiredGenerations,
          localWork,
          loginFailureSeen,
          loginFailurePreserved,
          turnOpen,
          lossThisTurn,
          postLossEvents,
          helperCalls,
          generationReused,
          wrapBlocked

vars ==
    <<phase, channelUp, confReceived, generationValid, generation,
      settledValid, settledGeneration, cleanupAttempts,
      attemptFailureCleanups, unexpectedShutdownCleanups,
      lostGenerations, cleanedGenerations, retiredGenerations,
      localWork, loginFailureSeen, loginFailurePreserved,
      turnOpen, lossThisTurn, postLossEvents, helperCalls,
      generationReused, wrapBlocked>>

CandidateGeneration ==
    IF ~generationValid THEN InitialGeneration ELSE NextGeneration(generation)

Init ==
    /\ phase = "Disconnected"
    /\ channelUp = FALSE
    /\ confReceived = FALSE
    /\ generationValid = FALSE
    /\ generation = InitialGeneration
    /\ settledValid = MutantNumericSentinel
    /\ settledGeneration = MaxGeneration
    /\ cleanupAttempts = 0
    /\ attemptFailureCleanups = 0
    /\ unexpectedShutdownCleanups = 0
    /\ lostGenerations = {}
    /\ cleanedGenerations = {}
    /\ retiredGenerations = {}
    /\ localWork = TRUE
    /\ loginFailureSeen = FALSE
    /\ loginFailurePreserved = TRUE
    /\ turnOpen = TRUE
    /\ lossThisTurn = FALSE
    /\ postLossEvents = 0
    /\ helperCalls = 0
    /\ generationReused = FALSE
    /\ wrapBlocked = FALSE

BeginLogin ==
    /\ phase = "Disconnected"
    /\ turnOpen
    /\ ~lossThisTurn
    /\ ~wrapBlocked
    /\ channelUp' = TRUE
    /\ confReceived' = FALSE
    /\ IF MutantActivateBeforeConf
          THEN /\ phase' = "Active"
               /\ generation' = CandidateGeneration
               /\ generationValid' = TRUE
               /\ generationReused' =
                     (generationReused
                      \/ (generationValid /\ generation = MaxGeneration))
          ELSE /\ phase' = "LoginAttempt"
               /\ UNCHANGED <<generation, generationValid,
                               generationReused>>
    /\ UNCHANGED <<settledValid, settledGeneration, cleanupAttempts,
                    attemptFailureCleanups, unexpectedShutdownCleanups,
                    lostGenerations, cleanedGenerations,
                    retiredGenerations, localWork, loginFailureSeen,
                    loginFailurePreserved, turnOpen, lossThisTurn,
                    postLossEvents, helperCalls, wrapBlocked>>

ReceiveConf ==
    /\ phase = "LoginAttempt"
    /\ channelUp
    /\ turnOpen
    /\ phase' = "Active"
    /\ confReceived' = TRUE
    /\ generation' = CandidateGeneration
    /\ generationValid' = TRUE
    /\ generationReused' =
          (generationReused
           \/ (generationValid /\ generation = MaxGeneration))
    /\ UNCHANGED <<channelUp, settledValid, settledGeneration,
                    cleanupAttempts, attemptFailureCleanups,
                    unexpectedShutdownCleanups, lostGenerations,
                    cleanedGenerations, retiredGenerations, localWork,
                    loginFailureSeen, loginFailurePreserved, turnOpen,
                    lossThisTurn, postLossEvents, helperCalls,
                    wrapBlocked>>

FailLoginAttempt ==
    /\ channelUp
    /\ ~confReceived
    /\ phase \in {"LoginAttempt", "Active"}
    /\ phase' = "Disconnected"
    /\ channelUp' = FALSE
    /\ confReceived' = FALSE
    /\ loginFailureSeen' = TRUE
    /\ IF MutantActivateBeforeConf
          THEN /\ localWork' = FALSE
               /\ loginFailurePreserved' = FALSE
               /\ attemptFailureCleanups' =
                     CapInc(attemptFailureCleanups)
               /\ cleanupAttempts' = CapInc(cleanupAttempts)
          ELSE /\ localWork' = localWork
               /\ loginFailurePreserved' = loginFailurePreserved
               /\ attemptFailureCleanups' = attemptFailureCleanups
               /\ cleanupAttempts' = cleanupAttempts
    /\ UNCHANGED <<generationValid, generation, settledValid,
                    settledGeneration, unexpectedShutdownCleanups,
                    lostGenerations, cleanedGenerations,
                    retiredGenerations, turnOpen, lossThisTurn,
                    postLossEvents, helperCalls, generationReused,
                    wrapBlocked>>

ActiveChannelLoss ==
    /\ phase = "Active"
    /\ channelUp
    /\ confReceived
    /\ generationValid
    /\ turnOpen
    /\ ~lossThisTurn
    /\ phase' = "LossPending"
    /\ channelUp' = FALSE
    /\ confReceived' = FALSE
    /\ lossThisTurn' = TRUE
    /\ lostGenerations' = lostGenerations \cup {generation}
    /\ UNCHANGED <<generationValid, generation, settledValid,
                    settledGeneration, cleanupAttempts,
                    attemptFailureCleanups, unexpectedShutdownCleanups,
                    cleanedGenerations, retiredGenerations, localWork,
                    loginFailureSeen, loginFailurePreserved, turnOpen,
                    postLossEvents, helperCalls, generationReused,
                    wrapBlocked>>

SameTurnWork ==
    /\ turnOpen
    /\ phase # "Shutdown"
    /\ lossThisTurn
    /\ MutantAllowPostLossWork
    /\ postLossEvents' = CapInc(postLossEvents)
    /\ UNCHANGED <<phase, channelUp, confReceived, generationValid,
                    generation, settledValid, settledGeneration,
                    cleanupAttempts, attemptFailureCleanups,
                    unexpectedShutdownCleanups, lostGenerations,
                    cleanedGenerations, retiredGenerations, localWork,
                    loginFailureSeen, loginFailurePreserved, turnOpen,
                    lossThisTurn, helperCalls, generationReused,
                    wrapBlocked>>

FinishLoss ==
    /\ turnOpen
    /\ lossThisTurn
    /\ phase \in {"LossPending", "Disconnected"}
    /\ generationValid
    /\ helperCalls' = CapInc(helperCalls)
    /\ phase' = "Disconnected"
    /\ channelUp' = FALSE
    /\ confReceived' = FALSE
    /\ retiredGenerations' = retiredGenerations \cup {generation}
    /\ wrapBlocked' =
          (wrapBlocked \/ (generation = MaxGeneration /\ ~MutantWrapGeneration))
    /\ IF settledValid /\ settledGeneration = generation
          THEN /\ settledValid' = settledValid
               /\ settledGeneration' = settledGeneration
               /\ cleanedGenerations' = cleanedGenerations
               /\ cleanupAttempts' =
                     IF MutantDuplicateCleanup
                     THEN CapInc(cleanupAttempts)
                     ELSE cleanupAttempts
               /\ localWork' = localWork
          ELSE /\ settledValid' = TRUE
               /\ settledGeneration' = generation
               /\ cleanedGenerations' =
                     cleanedGenerations \cup {generation}
               /\ cleanupAttempts' = CapInc(cleanupAttempts)
               /\ localWork' = FALSE
    /\ UNCHANGED <<generationValid, generation,
                    attemptFailureCleanups, unexpectedShutdownCleanups,
                    lostGenerations, loginFailureSeen,
                    loginFailurePreserved, turnOpen, lossThisTurn,
                    postLossEvents, generationReused>>

EndTurn ==
    /\ turnOpen
    /\ (~lossThisTurn \/ phase = "Disconnected")
    /\ turnOpen' = FALSE
    /\ lossThisTurn' = FALSE
    /\ UNCHANGED <<phase, channelUp, confReceived, generationValid,
                    generation, settledValid, settledGeneration,
                    cleanupAttempts, attemptFailureCleanups,
                    unexpectedShutdownCleanups, lostGenerations,
                    cleanedGenerations, retiredGenerations, localWork,
                    loginFailureSeen, loginFailurePreserved,
                    postLossEvents, helperCalls, generationReused,
                    wrapBlocked>>

StartTurn ==
    /\ ~turnOpen
    /\ phase # "Shutdown"
    /\ turnOpen' = TRUE
    /\ UNCHANGED <<phase, channelUp, confReceived, generationValid,
                    generation, settledValid, settledGeneration,
                    cleanupAttempts, attemptFailureCleanups,
                    unexpectedShutdownCleanups, lostGenerations,
                    cleanedGenerations, retiredGenerations, localWork,
                    loginFailureSeen, loginFailurePreserved,
                    lossThisTurn, postLossEvents, helperCalls,
                    generationReused, wrapBlocked>>

OrderlyShutdown ==
    /\ phase \in {"Disconnected", "LoginAttempt", "Active"}
    /\ turnOpen
    /\ ~lossThisTurn
    /\ phase' = "Shutdown"
    /\ channelUp' = FALSE
    /\ confReceived' = FALSE
    /\ localWork' = FALSE
    /\ unexpectedShutdownCleanups' =
          IF MutantShutdownAsLoss
          THEN CapInc(unexpectedShutdownCleanups)
          ELSE unexpectedShutdownCleanups
    /\ cleanupAttempts' =
          IF MutantShutdownAsLoss
          THEN CapInc(cleanupAttempts)
          ELSE cleanupAttempts
    /\ UNCHANGED <<generationValid, generation, settledValid,
                    settledGeneration, attemptFailureCleanups,
                    lostGenerations, cleanedGenerations,
                    retiredGenerations, loginFailureSeen,
                    loginFailurePreserved, turnOpen, lossThisTurn,
                    postLossEvents, helperCalls, generationReused,
                    wrapBlocked>>

Next ==
    \/ BeginLogin
    \/ ReceiveConf
    \/ FailLoginAttempt
    \/ ActiveChannelLoss
    \/ SameTurnWork
    \/ FinishLoss
    \/ EndTurn
    \/ StartTurn
    \/ OrderlyShutdown

Spec == Init /\ [][Next]_vars
FixedFairSpec == Spec /\ WF_vars(FinishLoss)

TypeOK ==
    /\ phase \in Phases
    /\ channelUp \in BOOLEAN
    /\ confReceived \in BOOLEAN
    /\ generationValid \in BOOLEAN
    /\ generation \in Generations
    /\ settledValid \in BOOLEAN
    /\ settledGeneration \in Generations
    /\ cleanupAttempts \in 0..CountCap
    /\ attemptFailureCleanups \in 0..CountCap
    /\ unexpectedShutdownCleanups \in 0..CountCap
    /\ lostGenerations \subseteq Generations
    /\ cleanedGenerations \subseteq Generations
    /\ retiredGenerations \subseteq Generations
    /\ localWork \in BOOLEAN
    /\ loginFailureSeen \in BOOLEAN
    /\ loginFailurePreserved \in BOOLEAN
    /\ turnOpen \in BOOLEAN
    /\ lossThisTurn \in BOOLEAN
    /\ postLossEvents \in 0..CountCap
    /\ helperCalls \in 0..CountCap
    /\ generationReused \in BOOLEAN
    /\ wrapBlocked \in BOOLEAN

ActiveRequiresConf ==
    phase = "Active" => channelUp /\ confReceived /\ generationValid

AttemptFailurePreserved ==
    /\ loginFailurePreserved
    /\ attemptFailureCleanups = 0

NoPostLossWork == postLossEvents = 0

CleanupCountCoherent ==
    cleanupAttempts = Cardinality(cleanedGenerations)

RetiredOnlyIfCleaned ==
    retiredGenerations \subseteq cleanedGenerations

CleanedOnlyIfLost ==
    cleanedGenerations \subseteq lostGenerations

NoUnexpectedShutdownCleanup == unexpectedShutdownCleanups = 0

NoGenerationReuse == ~generationReused

LossStateCoherent ==
    phase = "LossPending"
    => /\ lossThisTurn
       /\ ~channelUp
       /\ generation \in lostGenerations

SafetyInvariant ==
    /\ TypeOK
    /\ ActiveRequiresConf
    /\ AttemptFailurePreserved
    /\ NoPostLossWork
    /\ CleanupCountCoherent
    /\ RetiredOnlyIfCleaned
    /\ CleanedOnlyIfLost
    /\ NoUnexpectedShutdownCleanup
    /\ NoGenerationReuse
    /\ LossStateCoherent

LossEventuallySettled ==
    [](phase = "LossPending"
       => <> (phase = "Disconnected"
              /\ generation \in retiredGenerations))

NeverActive == phase # "Active"
NoLoginFailureSeen == ~loginFailureSeen
NeverRetiredLoss == retiredGenerations = {}

=============================================================================
