--------------------- MODULE MixedVersionCompatibility ---------------------
(***************************************************************************
Finite mixed-version scheduler/daemon/client compatibility model.

The scheduler is new. Client and fulfillment-daemon links negotiate OldVersion
or NewVersion independently. Assignment policy is selected before dispatch and
per assignment:

  old F, any compatible C       -> Legacy
  new F + old C                 -> FencedLegacy (epoch-scoped identity)
  new F + new C                 -> Token (full assignment id + nonce)

New C remains capable of the legacy projection when assigned to old F. The
model explicitly does not claim restart-exact identity for Legacy or
FencedLegacy: a delayed old claim after scheduler restart remains ambiguous.
Token assignments reject that stale claim exactly.

ProjectedTrace records only vocabulary visible to an old peer. PREPARE,
REVOKE, full ids, and tokens are erased from the projection; the remaining
trace must stay in the frozen old protocol language.
***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Clients, Workers, NoClient, NoWorker,
          OldVersion, NewVersion,
          ClientVersion, WorkerVersion,
          MutantPrepareOldWorker,
          MutantTokenOldClient,
          MutantGlobalCapability,
          MutantDefaultAllowUnknown,
          MutantAcceptStaleToken,
          MutantWaitReadyOldWorker,
          MutantPartialOldFrame,
          MutantNewTerminalOldPeer,
          MutantChoosePolicyAfterDispatch

ASSUME /\ IsFiniteSet(Clients)
       /\ Clients # {}
       /\ IsFiniteSet(Workers)
       /\ Workers # {}
       /\ NoClient \notin Clients
       /\ NoWorker \notin Workers
       /\ OldVersion # NewVersion
       /\ ClientVersion \in [Clients -> {OldVersion, NewVersion}]
       /\ WorkerVersion \in [Workers -> {OldVersion, NewVersion}]
       /\ MutantPrepareOldWorker \in BOOLEAN
       /\ MutantTokenOldClient \in BOOLEAN
       /\ MutantGlobalCapability \in BOOLEAN
       /\ MutantDefaultAllowUnknown \in BOOLEAN
       /\ MutantAcceptStaleToken \in BOOLEAN
       /\ MutantWaitReadyOldWorker \in BOOLEAN
       /\ MutantPartialOldFrame \in BOOLEAN
       /\ MutantNewTerminalOldPeer \in BOOLEAN
       /\ MutantChoosePolicyAfterDispatch \in BOOLEAN

Phases ==
    {"Idle", "Requested", "DispatchedUndecided", "Assigned",
     "WaitingReady", "UseCSDelivered", "Claimed", "Started",
     "Detached", "Restarted", "Terminal"}
Policies == {"None", "Undecided", "Legacy", "FencedLegacy", "Token"}
Guarantees == {"None", "LegacyOnly", "EpochScoped", "ExactRestart"}
ClaimIdentities == {"None", "Wire", "FullToken"}
OldEvents == {"REQUEST", "ASSIGN", "USECS", "CLAIM", "BEGIN",
              "DONE", "CANCEL", "LOSS"}

ExpectedPolicy(c, w) ==
    IF WorkerVersion[w] = OldVersion
    THEN "Legacy"
    ELSE IF ClientVersion[c] = OldVersion
         THEN "FencedLegacy"
         ELSE "Token"

GuaranteeOf(p) ==
    CASE p = "Legacy" -> "LegacyOnly"
      [] p = "FencedLegacy" -> "EpochScoped"
      [] p = "Token" -> "ExactRestart"
      [] OTHER -> "None"

IdentityOf(p) == IF p = "Token" THEN "FullToken" ELSE "Wire"

OldTraceLanguage ==
    {<<>>,
     <<"REQUEST">>,
     <<"REQUEST", "ASSIGN">>,
     <<"REQUEST", "ASSIGN", "USECS">>,
     <<"REQUEST", "ASSIGN", "USECS", "CLAIM">>,
     <<"REQUEST", "ASSIGN", "USECS", "CLAIM", "BEGIN">>,
     <<"REQUEST", "ASSIGN", "USECS", "CLAIM", "BEGIN", "DONE">>,
     <<"REQUEST", "ASSIGN", "CANCEL">>,
     <<"REQUEST", "ASSIGN", "LOSS">>,
     <<"REQUEST", "ASSIGN", "USECS", "CANCEL">>,
     <<"REQUEST", "ASSIGN", "USECS", "LOSS">>,
     <<"REQUEST", "ASSIGN", "USECS", "CLAIM", "LOSS">>,
     <<"REQUEST", "ASSIGN", "USECS", "CLAIM", "BEGIN", "LOSS">>}

VARIABLES phase,
          activeClient,
          activeWorker,
          expectedPolicy,
          policy,
          guarantee,
          claimIdentity,
          schedulerEpoch,
          projectedTrace,
          terminalCount,
          released,
          compacted,
          globalCapability,
          seenLegacy,
          seenFenced,
          seenToken,
          normalCompleted,
          cancelPreDeliverySeen,
          revokeBeforeClaimSeen,
          legacyRestartAmbiguous,
          fencedRestartAmbiguous,
          staleTokenRejected,
          workerLossHandled,
          submitterLossHandled,
          detachedCompletionSeen,
          oldProjectionComplete,
          oldWorkerSawNewControl,
          oldClientSawToken,
          capabilityLeak,
          startAfterRelease,
          staleTokenAccepted,
          oldPermanentWait,
          partialOldFrame,
          newTerminalToOld,
          policyChosenAfterDispatch

vars ==
    <<phase, activeClient, activeWorker, expectedPolicy, policy,
      guarantee, claimIdentity, schedulerEpoch, projectedTrace,
      terminalCount, released, compacted, globalCapability,
      seenLegacy, seenFenced, seenToken, normalCompleted,
      cancelPreDeliverySeen, revokeBeforeClaimSeen,
      legacyRestartAmbiguous, fencedRestartAmbiguous,
      staleTokenRejected, workerLossHandled, submitterLossHandled,
      detachedCompletionSeen, oldProjectionComplete,
      oldWorkerSawNewControl, oldClientSawToken, capabilityLeak,
      startAfterRelease, staleTokenAccepted, oldPermanentWait,
      partialOldFrame, newTerminalToOld, policyChosenAfterDispatch>>

Init ==
    /\ phase = "Idle"
    /\ activeClient = NoClient
    /\ activeWorker = NoWorker
    /\ expectedPolicy = "None"
    /\ policy = "None"
    /\ guarantee = "None"
    /\ claimIdentity = "None"
    /\ schedulerEpoch = 0
    /\ projectedTrace = <<>>
    /\ terminalCount = 0
    /\ released = FALSE
    /\ compacted = FALSE
    /\ globalCapability = "None"
    /\ seenLegacy = FALSE
    /\ seenFenced = FALSE
    /\ seenToken = FALSE
    /\ normalCompleted = FALSE
    /\ cancelPreDeliverySeen = FALSE
    /\ revokeBeforeClaimSeen = FALSE
    /\ legacyRestartAmbiguous = FALSE
    /\ fencedRestartAmbiguous = FALSE
    /\ staleTokenRejected = FALSE
    /\ workerLossHandled = FALSE
    /\ submitterLossHandled = FALSE
    /\ detachedCompletionSeen = FALSE
    /\ oldProjectionComplete = FALSE
    /\ oldWorkerSawNewControl = FALSE
    /\ oldClientSawToken = FALSE
    /\ capabilityLeak = FALSE
    /\ startAfterRelease = FALSE
    /\ staleTokenAccepted = FALSE
    /\ oldPermanentWait = FALSE
    /\ partialOldFrame = FALSE
    /\ newTerminalToOld = FALSE
    /\ policyChosenAfterDispatch = FALSE

Request(c) ==
    /\ c \in Clients
    /\ phase = "Idle"
    /\ phase' = "Requested"
    /\ activeClient' = c
    /\ activeWorker' = NoWorker
    /\ expectedPolicy' = "None"
    /\ policy' = "None"
    /\ guarantee' = "None"
    /\ claimIdentity' = "None"
    /\ schedulerEpoch' = 0
    /\ projectedTrace' = <<"REQUEST">>
    /\ terminalCount' = 0
    /\ released' = FALSE
    /\ compacted' = FALSE
    /\ UNCHANGED <<globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

Assign(w) ==
    /\ w \in Workers
    /\ phase = "Requested"
    /\ LET expected == ExpectedPolicy(activeClient, w)
           chosen ==
               IF MutantGlobalCapability /\ globalCapability = "Token"
               THEN "Token"
               ELSE expected
           late == MutantChoosePolicyAfterDispatch
           sendPrepare ==
               ((WorkerVersion[w] = NewVersion)
                /\ chosen \in {"FencedLegacy", "Token"})
               \/ (MutantPrepareOldWorker
                   /\ WorkerVersion[w] = OldVersion)
           sendToken ==
               (chosen = "Token")
               \/ (MutantTokenOldClient
                   /\ ClientVersion[activeClient] = OldVersion)
       IN /\ activeWorker' = w
          /\ expectedPolicy' = expected
          /\ policy' = IF late THEN "Undecided" ELSE chosen
          /\ guarantee' = IF late THEN "None" ELSE GuaranteeOf(chosen)
          /\ phase' =
                IF late
                THEN "DispatchedUndecided"
                ELSE IF MutantWaitReadyOldWorker
                        /\ WorkerVersion[w] = OldVersion
                     THEN "WaitingReady"
                     ELSE "Assigned"
          /\ projectedTrace' = Append(projectedTrace, "ASSIGN")
          /\ seenLegacy' = seenLegacy \/ (~late /\ chosen = "Legacy")
          /\ seenFenced' = seenFenced \/ (~late /\ chosen = "FencedLegacy")
          /\ seenToken' = seenToken \/ (~late /\ chosen = "Token")
          /\ oldWorkerSawNewControl' =
                oldWorkerSawNewControl
                \/ (WorkerVersion[w] = OldVersion /\ sendPrepare)
          /\ oldClientSawToken' =
                oldClientSawToken
                \/ (ClientVersion[activeClient] = OldVersion /\ sendToken)
          /\ capabilityLeak' =
                capabilityLeak \/ (~late /\ chosen # expected)
          /\ oldPermanentWait' =
                oldPermanentWait
                \/ (MutantWaitReadyOldWorker
                    /\ WorkerVersion[w] = OldVersion)
          /\ policyChosenAfterDispatch' =
                policyChosenAfterDispatch \/ late
          /\ globalCapability' =
                IF ClientVersion[activeClient] = NewVersion
                   /\ WorkerVersion[w] = NewVersion
                THEN "Token"
                ELSE globalCapability
    /\ UNCHANGED <<claimIdentity, schedulerEpoch, terminalCount,
                    released, compacted, normalCompleted,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, startAfterRelease,
                    staleTokenAccepted, partialOldFrame,
                    newTerminalToOld>>

ChoosePolicyLate ==
    /\ phase = "DispatchedUndecided"
    /\ activeWorker \in Workers
    /\ expectedPolicy \in {"Legacy", "FencedLegacy", "Token"}
    /\ policy' = expectedPolicy
    /\ guarantee' = GuaranteeOf(expectedPolicy)
    /\ phase' =
          IF MutantWaitReadyOldWorker
             /\ WorkerVersion[activeWorker] = OldVersion
          THEN "WaitingReady"
          ELSE "Assigned"
    /\ seenLegacy' = (seenLegacy \/ expectedPolicy = "Legacy")
    /\ seenFenced' = (seenFenced \/ expectedPolicy = "FencedLegacy")
    /\ seenToken' = (seenToken \/ expectedPolicy = "Token")
    /\ oldPermanentWait' =
          oldPermanentWait
          \/ (MutantWaitReadyOldWorker
              /\ WorkerVersion[activeWorker] = OldVersion)
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy,
                    claimIdentity, schedulerEpoch, projectedTrace,
                    terminalCount, released, compacted,
                    globalCapability, normalCompleted,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, partialOldFrame,
                    newTerminalToOld, policyChosenAfterDispatch>>

DeliverUseCS ==
    /\ phase = "Assigned"
    /\ policy \in {"Legacy", "FencedLegacy", "Token"}
    /\ phase' = "UseCSDelivered"
    /\ claimIdentity' = IdentityOf(policy)
    /\ projectedTrace' = Append(projectedTrace, "USECS")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, schedulerEpoch, terminalCount, released,
                    compacted, globalCapability, seenLegacy, seenFenced,
                    seenToken, normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

ConsumeOldFramePartially ==
    /\ phase = "UseCSDelivered"
    /\ MutantPartialOldFrame
    /\ (ClientVersion[activeClient] = OldVersion
        \/ WorkerVersion[activeWorker] = OldVersion)
    /\ partialOldFrame' = TRUE
    /\ UNCHANGED <<phase, activeClient, activeWorker, expectedPolicy,
                    policy, guarantee, claimIdentity, schedulerEpoch,
                    projectedTrace, terminalCount, released, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    newTerminalToOld, policyChosenAfterDispatch>>

ClaimAtWorker ==
    /\ phase = "UseCSDelivered"
    /\ claimIdentity = IdentityOf(policy)
    /\ phase' = "Claimed"
    /\ projectedTrace' = Append(projectedTrace, "CLAIM")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch,
                    terminalCount, released, compacted, globalCapability,
                    seenLegacy, seenFenced, seenToken, normalCompleted,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

StartCompile ==
    /\ phase = "Claimed"
    /\ phase' = "Started"
    /\ projectedTrace' = Append(projectedTrace, "BEGIN")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch,
                    terminalCount, released, compacted, globalCapability,
                    seenLegacy, seenFenced, seenToken, normalCompleted,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

CompleteCompile ==
    /\ phase = "Started"
    /\ LET nextTrace == Append(projectedTrace, "DONE")
       IN /\ phase' = "Terminal"
          /\ terminalCount' = terminalCount + 1
          /\ released' = TRUE
          /\ normalCompleted' = TRUE
          /\ projectedTrace' = nextTrace
          /\ oldProjectionComplete' =
                oldProjectionComplete \/ nextTrace \in OldTraceLanguage
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

CancelBeforeDelivery ==
    /\ phase = "Assigned"
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ released' = TRUE
    /\ cancelPreDeliverySeen' = TRUE
    /\ projectedTrace' = Append(projectedTrace, "CANCEL")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

RevokeBeforeClaim ==
    /\ phase = "UseCSDelivered"
    /\ WorkerVersion[activeWorker] = NewVersion
    /\ policy \in {"FencedLegacy", "Token"}
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ released' = TRUE
    /\ revokeBeforeClaimSeen' = TRUE
    /\ projectedTrace' = Append(projectedTrace, "CANCEL")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

RestartScheduler ==
    /\ phase = "UseCSDelivered"
    /\ schedulerEpoch = 0
    /\ phase' = "Restarted"
    /\ schedulerEpoch' = 1
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, projectedTrace,
                    terminalCount, released, compacted, globalCapability,
                    seenLegacy, seenFenced, seenToken, normalCompleted,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

DelayedClaimAfterRestart ==
    /\ phase = "Restarted"
    /\ schedulerEpoch = 1
    /\ IF policy = "Token"
          THEN IF MutantAcceptStaleToken
               THEN /\ phase' = "Claimed"
                    /\ staleTokenAccepted' = TRUE
                    /\ staleTokenRejected' = staleTokenRejected
                    /\ projectedTrace' = Append(projectedTrace, "CLAIM")
                    /\ terminalCount' = terminalCount
                    /\ released' = released
               ELSE /\ phase' = "Terminal"
                    /\ staleTokenRejected' = TRUE
                    /\ staleTokenAccepted' = staleTokenAccepted
                    /\ projectedTrace' = Append(projectedTrace, "CANCEL")
                    /\ terminalCount' = terminalCount + 1
                    /\ released' = TRUE
          ELSE /\ phase' = "Claimed"
               /\ staleTokenAccepted' = staleTokenAccepted
               /\ staleTokenRejected' = staleTokenRejected
               /\ projectedTrace' = Append(projectedTrace, "CLAIM")
               /\ terminalCount' = terminalCount
               /\ released' = released
    /\ legacyRestartAmbiguous' =
          (legacyRestartAmbiguous \/ policy = "Legacy")
    /\ fencedRestartAmbiguous' =
          (fencedRestartAmbiguous \/ policy = "FencedLegacy")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, workerLossHandled,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    oldPermanentWait, partialOldFrame,
                    newTerminalToOld, policyChosenAfterDispatch>>

WorkerSessionLoss ==
    /\ phase \in {"Claimed", "Started", "Detached"}
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ released' = TRUE
    /\ workerLossHandled' = TRUE
    /\ projectedTrace' = Append(projectedTrace, "LOSS")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    submitterLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

SubmitterLossBeforeStart ==
    /\ phase \in {"Assigned", "UseCSDelivered", "Claimed"}
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ released' = TRUE
    /\ submitterLossHandled' = TRUE
    /\ projectedTrace' = Append(projectedTrace, "LOSS")
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

DetachStartedSubmitter ==
    /\ phase = "Started"
    /\ phase' = "Detached"
    /\ submitterLossHandled' = TRUE
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch,
                    projectedTrace, terminalCount, released, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, detachedCompletionSeen,
                    oldProjectionComplete, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

CompleteDetached ==
    /\ phase = "Detached"
    /\ LET nextTrace == Append(projectedTrace, "DONE")
       IN /\ phase' = "Terminal"
          /\ terminalCount' = terminalCount + 1
          /\ released' = TRUE
          /\ detachedCompletionSeen' = TRUE
          /\ normalCompleted' = TRUE
          /\ projectedTrace' = nextTrace
          /\ oldProjectionComplete' =
                oldProjectionComplete \/ nextTrace \in OldTraceLanguage
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    cancelPreDeliverySeen, revokeBeforeClaimSeen,
                    legacyRestartAmbiguous, fencedRestartAmbiguous,
                    staleTokenRejected, workerLossHandled,
                    submitterLossHandled, oldWorkerSawNewControl,
                    oldClientSawToken, capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

CompactTerminalRecord ==
    /\ phase = "Terminal"
    /\ released
    /\ ~compacted
    /\ compacted' = TRUE
    /\ UNCHANGED <<phase, activeClient, activeWorker, expectedPolicy,
                    policy, guarantee, claimIdentity, schedulerEpoch,
                    projectedTrace, terminalCount, released,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

DefaultAllowUnknownClaim ==
    /\ phase = "Terminal"
    /\ compacted
    /\ released
    /\ MutantDefaultAllowUnknown
    /\ phase' = "Started"
    /\ startAfterRelease' = TRUE
    /\ UNCHANGED <<activeClient, activeWorker, expectedPolicy, policy,
                    guarantee, claimIdentity, schedulerEpoch,
                    projectedTrace, terminalCount, released, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

SendNewTerminalFieldToOldPeer ==
    /\ phase = "Terminal"
    /\ MutantNewTerminalOldPeer
    /\ (ClientVersion[activeClient] = OldVersion
        \/ WorkerVersion[activeWorker] = OldVersion)
    /\ newTerminalToOld' = TRUE
    /\ UNCHANGED <<phase, activeClient, activeWorker, expectedPolicy,
                    policy, guarantee, claimIdentity, schedulerEpoch,
                    projectedTrace, terminalCount, released, compacted,
                    globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, policyChosenAfterDispatch>>

ResetAssignment ==
    /\ phase = "Terminal"
    /\ phase' = "Idle"
    /\ activeClient' = NoClient
    /\ activeWorker' = NoWorker
    /\ expectedPolicy' = "None"
    /\ policy' = "None"
    /\ guarantee' = "None"
    /\ claimIdentity' = "None"
    /\ schedulerEpoch' = 0
    /\ projectedTrace' = <<>>
    /\ terminalCount' = 0
    /\ released' = FALSE
    /\ compacted' = FALSE
    /\ UNCHANGED <<globalCapability, seenLegacy, seenFenced, seenToken,
                    normalCompleted, cancelPreDeliverySeen,
                    revokeBeforeClaimSeen, legacyRestartAmbiguous,
                    fencedRestartAmbiguous, staleTokenRejected,
                    workerLossHandled, submitterLossHandled,
                    detachedCompletionSeen, oldProjectionComplete,
                    oldWorkerSawNewControl, oldClientSawToken,
                    capabilityLeak, startAfterRelease,
                    staleTokenAccepted, oldPermanentWait,
                    partialOldFrame, newTerminalToOld,
                    policyChosenAfterDispatch>>

Next ==
    \/ \E c \in Clients : Request(c)
    \/ \E w \in Workers : Assign(w)
    \/ ChoosePolicyLate
    \/ DeliverUseCS
    \/ ConsumeOldFramePartially
    \/ ClaimAtWorker
    \/ StartCompile
    \/ CompleteCompile
    \/ CancelBeforeDelivery
    \/ RevokeBeforeClaim
    \/ RestartScheduler
    \/ DelayedClaimAfterRestart
    \/ WorkerSessionLoss
    \/ SubmitterLossBeforeStart
    \/ DetachStartedSubmitter
    \/ CompleteDetached
    \/ CompactTerminalRecord
    \/ DefaultAllowUnknownClaim
    \/ SendNewTerminalFieldToOldPeer
    \/ ResetAssignment

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in Phases
    /\ activeClient \in Clients \cup {NoClient}
    /\ activeWorker \in Workers \cup {NoWorker}
    /\ expectedPolicy \in Policies
    /\ policy \in Policies
    /\ guarantee \in Guarantees
    /\ claimIdentity \in ClaimIdentities
    /\ schedulerEpoch \in 0..1
    /\ projectedTrace \in Seq(OldEvents)
    /\ Len(projectedTrace) <= 6
    /\ terminalCount \in 0..1
    /\ released \in BOOLEAN
    /\ compacted \in BOOLEAN
    /\ globalCapability \in {"None", "Token"}
    /\ seenLegacy \in BOOLEAN
    /\ seenFenced \in BOOLEAN
    /\ seenToken \in BOOLEAN
    /\ normalCompleted \in BOOLEAN
    /\ cancelPreDeliverySeen \in BOOLEAN
    /\ revokeBeforeClaimSeen \in BOOLEAN
    /\ legacyRestartAmbiguous \in BOOLEAN
    /\ fencedRestartAmbiguous \in BOOLEAN
    /\ staleTokenRejected \in BOOLEAN
    /\ workerLossHandled \in BOOLEAN
    /\ submitterLossHandled \in BOOLEAN
    /\ detachedCompletionSeen \in BOOLEAN
    /\ oldProjectionComplete \in BOOLEAN
    /\ oldWorkerSawNewControl \in BOOLEAN
    /\ oldClientSawToken \in BOOLEAN
    /\ capabilityLeak \in BOOLEAN
    /\ startAfterRelease \in BOOLEAN
    /\ staleTokenAccepted \in BOOLEAN
    /\ oldPermanentWait \in BOOLEAN
    /\ partialOldFrame \in BOOLEAN
    /\ newTerminalToOld \in BOOLEAN
    /\ policyChosenAfterDispatch \in BOOLEAN

PerAssignmentPolicyCorrect ==
    phase \notin {"Idle", "Requested", "DispatchedUndecided"}
    => policy = expectedPolicy

GuaranteeMatchesPolicy == guarantee = GuaranteeOf(policy)

ClaimIdentityMatchesPolicy ==
    phase \in {"UseCSDelivered", "Claimed", "Started", "Detached",
               "Restarted", "Terminal"}
    => claimIdentity = IdentityOf(policy)

OldWorkerSeesOnlyOldVocabulary == ~oldWorkerSawNewControl
OldClientSeesNoTokenField == ~oldClientSawToken
NoGlobalCapabilityLeak == ~capabilityLeak
NoStartAfterRelease == ~startAfterRelease
TokenRejectsStaleRestartClaim == ~staleTokenAccepted
OldPeerHasNoNewHandshakeWait == ~oldPermanentWait
OldFrameFullyConsumed == ~partialOldFrame
OldPeerSeesNoNewTerminalField == ~newTerminalToOld
PolicyChosenBeforeDispatch == ~policyChosenAfterDispatch
OldTraceRefinement == projectedTrace \in OldTraceLanguage
TerminalAtMostOnce == terminalCount <= 1
ReleaseHasTerminalToken == released => terminalCount = 1

SafetyInvariant ==
    /\ TypeOK
    /\ PerAssignmentPolicyCorrect
    /\ GuaranteeMatchesPolicy
    /\ ClaimIdentityMatchesPolicy
    /\ OldWorkerSeesOnlyOldVocabulary
    /\ OldClientSeesNoTokenField
    /\ NoGlobalCapabilityLeak
    /\ NoStartAfterRelease
    /\ TokenRejectsStaleRestartClaim
    /\ OldPeerHasNoNewHandshakeWait
    /\ OldFrameFullyConsumed
    /\ OldPeerSeesNoNewTerminalField
    /\ PolicyChosenBeforeDispatch
    /\ OldTraceRefinement
    /\ TerminalAtMostOnce
    /\ ReleaseHasTerminalToken

NoLegacyAssignmentSeen == ~seenLegacy
NoFencedAssignmentSeen == ~seenFenced
NoTokenAssignmentSeen == ~seenToken
NoNormalCompletion == ~normalCompleted
NoCancelBeforeDelivery == ~cancelPreDeliverySeen
NoRevokeBeforeClaim == ~revokeBeforeClaimSeen
NoLegacyRestartAmbiguity == ~legacyRestartAmbiguous
NoFencedRestartAmbiguity == ~fencedRestartAmbiguous
NoTokenStaleReject == ~staleTokenRejected
NoWorkerLossHandled == ~workerLossHandled
NoSubmitterLossHandled == ~submitterLossHandled
NoDetachedCompletion == ~detachedCompletionSeen
NoMixedLegacyToken == ~((seenLegacy /\ seenToken))
NoMixedFencedToken == ~((seenFenced /\ seenToken))
NoOldProjectionCompletion == ~oldProjectionComplete

FirstClient == CHOOSE c \in Clients : TRUE
OtherClient == CHOOSE c \in Clients : c # FirstClient
FirstWorker == CHOOSE w \in Workers : TRUE
OtherWorker == CHOOSE w \in Workers : w # FirstWorker

AllOldClients == [c \in Clients |-> OldVersion]
AllNewClients == [c \in Clients |-> NewVersion]
MixedClients ==
    [c \in Clients |-> IF c = FirstClient THEN OldVersion ELSE NewVersion]
AllOldWorkers == [w \in Workers |-> OldVersion]
AllNewWorkers == [w \in Workers |-> NewVersion]
MixedWorkers ==
    [w \in Workers |-> IF w = FirstWorker THEN OldVersion ELSE NewVersion]

=============================================================================
