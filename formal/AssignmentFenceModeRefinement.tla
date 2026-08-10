-------------------- MODULE AssignmentFenceModeRefinement --------------------
(***************************************************************************
Small concrete/abstract refinement model for strict and pipelined enforcing.

The abstract machine exposes only authorization, revoke, start, terminal, and
release.  Pipelined client claims received before PREPARE consumption are
concrete pending records and must stutter abstractly.  PREPARE consumption may
resolve a pending exact claim and create the abstract authorization.  No
compiler/environment side effect may precede that authorization.
***************************************************************************)
EXTENDS Naturals, TLC

CONSTANTS StrictMode,
          PipelinedMode,
          Mode,
          MaxPending,
          MutantPendingAuthorizes,
          MutantStartBeforeGrant,
          MutantDropRevoke,
          MutantUnboundedPending

ASSUME /\ StrictMode # PipelinedMode
       /\ Mode \in {StrictMode, PipelinedMode}
       /\ MaxPending \in Nat \ {0}
       /\ MutantPendingAuthorizes \in BOOLEAN
       /\ MutantStartBeforeGrant \in BOOLEAN
       /\ MutantDropRevoke \in BOOLEAN
       /\ MutantUnboundedPending \in BOOLEAN

AbstractStates ==
    {"Idle", "Awaiting", "Authorized", "Revoked",
     "Started", "Terminal", "Released"}

CapInc(n) == IF n < MaxPending THEN n + 1 ELSE n

VARIABLES prepareQueued,
          prepared,
          readyObserved,
          usecsExposed,
          claimArrived,
          pendingCount,
          matched,
          authorizationSeen,
          revokeQueued,
          revoked,
          started,
          terminal,
          released,
          abstractState,
          pendingStutterSeen,
          rejectedClaims

vars ==
    <<prepareQueued, prepared, readyObserved, usecsExposed,
      claimArrived, pendingCount, matched, authorizationSeen,
      revokeQueued, revoked, started, terminal, released,
      abstractState, pendingStutterSeen, rejectedClaims>>

AbsOf ==
    IF released THEN "Released"
    ELSE IF terminal THEN "Terminal"
    ELSE IF started THEN "Started"
    ELSE IF revoked THEN "Revoked"
    ELSE IF matched THEN "Authorized"
    ELSE IF prepareQueued THEN "Awaiting"
    ELSE "Idle"

Init ==
    /\ prepareQueued = FALSE
    /\ prepared = FALSE
    /\ readyObserved = FALSE
    /\ usecsExposed = FALSE
    /\ claimArrived = FALSE
    /\ pendingCount = 0
    /\ matched = FALSE
    /\ authorizationSeen = FALSE
    /\ revokeQueued = FALSE
    /\ revoked = FALSE
    /\ started = FALSE
    /\ terminal = FALSE
    /\ released = FALSE
    /\ abstractState = "Idle"
    /\ pendingStutterSeen = FALSE
    /\ rejectedClaims = 0

QueuePrepare ==
    /\ ~prepareQueued
    /\ ~released
    /\ prepareQueued' = TRUE
    /\ abstractState' = "Awaiting"
    /\ UNCHANGED <<prepared, readyObserved, usecsExposed,
                    claimArrived, pendingCount, matched,
                    authorizationSeen, revokeQueued, revoked,
                    started, terminal, released,
                    pendingStutterSeen, rejectedClaims>>

ConsumePrepare ==
    /\ prepareQueued
    /\ ~prepared
    /\ ~revoked
    /\ prepared' = TRUE
    /\ LET resolvesPending == pendingCount > 0
       IN /\ matched' = matched \/ resolvesPending
          /\ authorizationSeen' = authorizationSeen \/ resolvesPending
          /\ pendingCount' = IF resolvesPending THEN 0 ELSE pendingCount
          /\ abstractState' =
                IF resolvesPending THEN "Authorized" ELSE abstractState
    /\ UNCHANGED <<prepareQueued, readyObserved, usecsExposed,
                    claimArrived, revokeQueued, revoked,
                    started, terminal, released,
                    pendingStutterSeen, rejectedClaims>>

ObserveReady ==
    /\ prepared
    /\ ~readyObserved
    /\ ~revoked
    /\ readyObserved' = TRUE
    /\ UNCHANGED <<prepareQueued, prepared, usecsExposed,
                    claimArrived, pendingCount, matched,
                    authorizationSeen, revokeQueued, revoked,
                    started, terminal, released, abstractState,
                    pendingStutterSeen, rejectedClaims>>

ExposeUseCS ==
    /\ ~usecsExposed
    /\ ~revoked
    /\ IF Mode = StrictMode THEN readyObserved ELSE prepareQueued
    /\ usecsExposed' = TRUE
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    claimArrived, pendingCount, matched,
                    authorizationSeen, revokeQueued, revoked,
                    started, terminal, released, abstractState,
                    pendingStutterSeen, rejectedClaims>>

ClientClaim ==
    /\ usecsExposed
    /\ ~claimArrived
    /\ ~released
    /\ claimArrived' = TRUE
    /\ IF prepared /\ ~revoked
          THEN /\ matched' = TRUE
               /\ authorizationSeen' = TRUE
               /\ pendingCount' = 0
               /\ abstractState' = "Authorized"
               /\ pendingStutterSeen' = pendingStutterSeen
               /\ rejectedClaims' = rejectedClaims
          ELSE IF Mode = PipelinedMode /\ ~prepared /\ prepareQueued
          THEN /\ matched' = FALSE
               /\ authorizationSeen' = authorizationSeen
               /\ pendingCount' =
                     IF pendingCount < MaxPending
                     THEN pendingCount + 1
                     ELSE pendingCount
               /\ abstractState' =
                     IF MutantPendingAuthorizes
                     THEN "Authorized"
                     ELSE abstractState
               /\ pendingStutterSeen' = TRUE
               /\ rejectedClaims' =
                     IF pendingCount < MaxPending
                     THEN rejectedClaims
                     ELSE CapInc(rejectedClaims)
          ELSE /\ matched' = matched
               /\ authorizationSeen' = authorizationSeen
               /\ pendingCount' = pendingCount
               /\ abstractState' = abstractState
               /\ pendingStutterSeen' = pendingStutterSeen
               /\ rejectedClaims' = CapInc(rejectedClaims)
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, revokeQueued, revoked,
                    started, terminal, released>>

UnknownClaim ==
    /\ Mode = PipelinedMode
    /\ usecsExposed
    /\ ~prepared
    /\ ~revoked
    /\ ~released
    /\ pendingCount <= MaxPending
    /\ claimArrived' = TRUE
    /\ pendingStutterSeen' = TRUE
    /\ matched' = FALSE
    /\ authorizationSeen' = authorizationSeen
    /\ abstractState' =
          IF MutantPendingAuthorizes THEN "Authorized" ELSE abstractState
    /\ IF MutantUnboundedPending
          THEN /\ pendingCount' = pendingCount + 1
               /\ rejectedClaims' = rejectedClaims
          ELSE IF pendingCount < MaxPending
          THEN /\ pendingCount' = pendingCount + 1
               /\ rejectedClaims' = rejectedClaims
          ELSE /\ pendingCount' = pendingCount
               /\ rejectedClaims' = CapInc(rejectedClaims)
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, revokeQueued, revoked,
                    started, terminal, released>>

QueueRevoke ==
    /\ prepareQueued
    /\ ~revokeQueued
    /\ ~released
    /\ revokeQueued' = TRUE
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, claimArrived, pendingCount,
                    matched, authorizationSeen, revoked,
                    started, terminal, released, abstractState,
                    pendingStutterSeen, rejectedClaims>>

ConsumeRevoke ==
    /\ revokeQueued
    /\ ~released
    /\ LET fenceWins == ~matched /\ ~started
       IN /\ revokeQueued' = FALSE
          /\ revoked' = revoked \/ fenceWins
          /\ pendingCount' = IF fenceWins THEN 0 ELSE pendingCount
          /\ abstractState' =
                IF fenceWins /\ ~MutantDropRevoke
                THEN "Revoked"
                ELSE abstractState
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, claimArrived, matched,
                    authorizationSeen, started, terminal, released,
                    pendingStutterSeen, rejectedClaims>>

StartCompile ==
    /\ ~started
    /\ ~terminal
    /\ ~released
    /\ ((matched /\ ~revoked)
        \/ (MutantStartBeforeGrant
            /\ claimArrived
            /\ pendingCount > 0
            /\ ~authorizationSeen))
    /\ started' = TRUE
    /\ abstractState' =
          IF matched /\ authorizationSeen
          THEN "Started"
          ELSE abstractState
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, claimArrived, pendingCount,
                    matched, authorizationSeen, revokeQueued,
                    revoked, terminal, released,
                    pendingStutterSeen, rejectedClaims>>

FinishCompile ==
    /\ started
    /\ ~terminal
    /\ terminal' = TRUE
    /\ abstractState' = "Terminal"
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, claimArrived, pendingCount,
                    matched, authorizationSeen, revokeQueued,
                    revoked, started, released,
                    pendingStutterSeen, rejectedClaims>>

Release ==
    /\ terminal
    /\ ~released
    /\ released' = TRUE
    /\ abstractState' = "Released"
    /\ UNCHANGED <<prepareQueued, prepared, readyObserved,
                    usecsExposed, claimArrived, pendingCount,
                    matched, authorizationSeen, revokeQueued,
                    revoked, started, terminal,
                    pendingStutterSeen, rejectedClaims>>

Next ==
    \/ QueuePrepare
    \/ ConsumePrepare
    \/ ObserveReady
    \/ ExposeUseCS
    \/ ClientClaim
    \/ UnknownClaim
    \/ QueueRevoke
    \/ ConsumeRevoke
    \/ StartCompile
    \/ FinishCompile
    \/ Release

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ WF_vars(ConsumePrepare)

TypeOK ==
    /\ prepareQueued \in BOOLEAN
    /\ prepared \in BOOLEAN
    /\ readyObserved \in BOOLEAN
    /\ usecsExposed \in BOOLEAN
    /\ claimArrived \in BOOLEAN
    /\ pendingCount \in 0..(MaxPending + 1)
    /\ matched \in BOOLEAN
    /\ authorizationSeen \in BOOLEAN
    /\ revokeQueued \in BOOLEAN
    /\ revoked \in BOOLEAN
    /\ started \in BOOLEAN
    /\ terminal \in BOOLEAN
    /\ released \in BOOLEAN
    /\ abstractState \in AbstractStates
    /\ pendingStutterSeen \in BOOLEAN
    /\ rejectedClaims \in 0..MaxPending

AbstractionRelation == abstractState = AbsOf
StartRequiresAuthorization == started => authorizationSeen
PendingDoesNotAuthorize ==
    pendingCount > 0 => ~matched /\ abstractState = "Awaiting"
RevokedExcludesStart == revoked => ~started
ReleaseAfterTerminal == released => terminal
PendingBound == pendingCount <= MaxPending

RefinementSafety ==
    /\ TypeOK
    /\ AbstractionRelation
    /\ StartRequiresAuthorization
    /\ PendingDoesNotAuthorize
    /\ RevokedExcludesStart
    /\ ReleaseAfterTerminal
    /\ PendingBound

PendingEventuallyResolved ==
    [](pendingCount > 0 => <> (matched \/ revoked))

NoPendingStutterSeen == ~pendingStutterSeen
NoRevocationSeen == ~revoked

=============================================================================
