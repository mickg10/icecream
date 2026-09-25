------------------------- MODULE Protocol50ReplacementReplay -------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Focused bounded lifecycle model for two gaps in PipelineRecovery:
  * an F-applied reset whose RESET_CONFIRM is lost, followed by reconnect and
    replay of the same reset operation/result; and
  * terminal same-F logical relationship retirement followed by creation and
    use of a fresh logical relationship.

F-store restart is a separate transition and identity dimension.  The model
does not equate logical relationship retirement with a new F-store GUID.
Topology parameters provide 2/1, 3/1, 4/1 and 1/2, 1/3, 1/4 witnesses;
non-target links are abstract unaffected peers whose progress is recorded.
These are finite reachability/safety checks, not fairness-based liveness.
***************************************************************************)

CONSTANTS CStores, FStores, TargetC, TargetF,
          MutantResetReplay, MutantReuseOldRelationship,
          MutantSameFChangesStore

ASSUME /\ CStores # {}
       /\ FStores # {}
       /\ TargetC \in CStores
       /\ TargetF \in FStores
       /\ MutantResetReplay \in BOOLEAN
       /\ MutantReuseOldRelationship \in BOOLEAN
       /\ MutantSameFChangesStore \in BOOLEAN

Links == CStores \X FStores
TargetLink == <<TargetC, TargetF>>
SiblingLinks == Links \ {TargetLink}

ResetPhases == {"Idle", "Requested", "Applied", "AckObserved",
                "ConfirmInFlight", "ConfirmLost", "ReplayPending",
                "ReplayReply", "ReplayAckObserved", "Confirmed"}
RelationPhases == {"Live", "Retired", "ReplacementLive",
                   "FRestarted", "RestartReplacementLive"}
ReplacementKinds == {"None", "SameF", "FRestart"}

VARIABLES resetPhase, resetOp, historyEpoch, resetHistorySnapshot,
          resetRelEpochSnapshot, resetStoreGenerationSnapshot,
          replayedReset, replayHistory, replayRelEpoch, replayStoreGeneration,
          confirmAttempts, confirmDelivered, relationPhase, logicalId,
          relationshipEpoch, fStoreGeneration, retiredLogicalId,
          retiredStoreGeneration, replacementKind,
          replacementStoreGeneration, staleOldOfferAccepted,
          staleOldOfferRejected, replacementUsed, committedLogicalId,
          committedRelationshipEpoch, committedStoreGeneration,
          siblingProgress

vars == <<resetPhase, resetOp, historyEpoch, resetHistorySnapshot,
          resetRelEpochSnapshot, resetStoreGenerationSnapshot,
          replayedReset, replayHistory, replayRelEpoch, replayStoreGeneration,
          confirmAttempts, confirmDelivered, relationPhase, logicalId,
          relationshipEpoch, fStoreGeneration, retiredLogicalId,
          retiredStoreGeneration, replacementKind,
          replacementStoreGeneration, staleOldOfferAccepted,
          staleOldOfferRejected, replacementUsed, committedLogicalId,
          committedRelationshipEpoch, committedStoreGeneration,
          siblingProgress>>

Init ==
    /\ resetPhase = "Idle"
    /\ resetOp = 0
    /\ historyEpoch = 0
    /\ resetHistorySnapshot = 0
    /\ resetRelEpochSnapshot = 0
    /\ resetStoreGenerationSnapshot = 0
    /\ replayedReset = FALSE
    /\ replayHistory = 0
    /\ replayRelEpoch = 0
    /\ replayStoreGeneration = 0
    /\ confirmAttempts = 0
    /\ confirmDelivered = FALSE
    /\ relationPhase = "Live"
    /\ logicalId = 0
    /\ relationshipEpoch = 0
    /\ fStoreGeneration = 0
    /\ retiredLogicalId = 0
    /\ retiredStoreGeneration = 0
    /\ replacementKind = "None"
    /\ replacementStoreGeneration = 0
    /\ staleOldOfferAccepted = FALSE
    /\ staleOldOfferRejected = FALSE
    /\ replacementUsed = FALSE
    /\ committedLogicalId = 0
    /\ committedRelationshipEpoch = 0
    /\ committedStoreGeneration = 0
    /\ siblingProgress = {}

RequestReset ==
    /\ relationPhase = "Live"
    /\ resetPhase = "Idle"
    /\ resetPhase' = "Requested"
    /\ resetOp' = 1
    /\ UNCHANGED <<historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

ApplyReset ==
    /\ resetPhase = "Requested"
    /\ resetOp = 1
    /\ resetPhase' = "Applied"
    /\ historyEpoch' = historyEpoch + 1
    /\ resetHistorySnapshot' = historyEpoch + 1
    /\ relationshipEpoch' = relationshipEpoch + 1
    /\ resetRelEpochSnapshot' = relationshipEpoch + 1
    /\ resetStoreGenerationSnapshot' = fStoreGeneration
    /\ UNCHANGED <<resetOp, replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    relationPhase, logicalId,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

ObserveResetAck ==
    /\ resetPhase = "Applied"
    /\ resetPhase' = "AckObserved"
    /\ UNCHANGED <<resetOp, historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

SendResetConfirm ==
    /\ resetPhase \in {"AckObserved", "ReplayAckObserved"}
    /\ confirmAttempts < 2
    /\ resetPhase' = "ConfirmInFlight"
    /\ confirmAttempts' = confirmAttempts + 1
    /\ UNCHANGED <<resetOp, historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

LoseResetConfirm ==
    /\ resetPhase = "ConfirmInFlight"
    /\ ~confirmDelivered
    /\ resetPhase' = "ConfirmLost"
    /\ UNCHANGED <<resetOp, historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

ReconnectAfterLostConfirm ==
    /\ resetPhase = "ConfirmLost"
    /\ resetPhase' = "ReplayPending"
    /\ UNCHANGED <<resetOp, historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

ReplayPriorReset ==
    /\ resetPhase = "ReplayPending"
    /\ resetOp = 1
    /\ resetPhase' = "ReplayReply"
    /\ historyEpoch' = IF MutantResetReplay
                           THEN historyEpoch + 1 ELSE historyEpoch
    /\ replayedReset' = TRUE
    /\ replayHistory' = IF MutantResetReplay
                           THEN historyEpoch + 1 ELSE historyEpoch
    /\ replayRelEpoch' = relationshipEpoch
    /\ replayStoreGeneration' = fStoreGeneration
    /\ UNCHANGED <<resetOp, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    confirmAttempts, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

ObserveReplayAck ==
    /\ resetPhase = "ReplayReply"
    /\ resetPhase' = "ReplayAckObserved"
    /\ UNCHANGED <<resetOp, historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

ProcessResetConfirm ==
    /\ resetPhase = "ConfirmInFlight"
    /\ ~confirmDelivered
    /\ resetPhase' = "Confirmed"
    /\ confirmDelivered' = TRUE
    /\ UNCHANGED <<resetOp, historyEpoch, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts,
                    relationPhase, logicalId, relationshipEpoch,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

RetireSameFRelationship ==
    /\ relationPhase = "Live"
    /\ resetPhase = "Confirmed"
    /\ logicalId = 0
    /\ relationshipEpoch < 2
    /\ relationPhase' = "Retired"
    /\ retiredLogicalId' = logicalId
    /\ retiredStoreGeneration' = fStoreGeneration
    /\ UNCHANGED <<resetPhase, resetOp, historyEpoch,
                    resetHistorySnapshot, resetRelEpochSnapshot,
                    resetStoreGenerationSnapshot, replayedReset,
                    replayHistory, replayRelEpoch, replayStoreGeneration,
                    confirmAttempts, confirmDelivered, logicalId,
                    relationshipEpoch,
                    fStoreGeneration, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

CreateSameFReplacement ==
    /\ relationPhase = "Retired"
    /\ logicalId = retiredLogicalId
    /\ relationshipEpoch < 2
    /\ relationPhase' = "ReplacementLive"
    /\ logicalId' = IF MutantReuseOldRelationship
                       THEN retiredLogicalId ELSE retiredLogicalId + 1
    /\ relationshipEpoch' = IF MutantReuseOldRelationship
                               THEN relationshipEpoch
                               ELSE relationshipEpoch + 1
    /\ replacementKind' = "SameF"
    /\ replacementStoreGeneration' =
           IF MutantSameFChangesStore THEN fStoreGeneration + 1
           ELSE fStoreGeneration
    /\ fStoreGeneration' = replacementStoreGeneration'
    /\ historyEpoch' = 0
    /\ UNCHANGED <<resetPhase, resetOp, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    retiredLogicalId, retiredStoreGeneration,
                    staleOldOfferAccepted, staleOldOfferRejected,
                    replacementUsed, committedLogicalId,
                    committedRelationshipEpoch, committedStoreGeneration,
                    siblingProgress>>

TryOldRelationshipOffer ==
    /\ relationPhase = "ReplacementLive"
    /\ ~staleOldOfferAccepted
    /\ IF /\ retiredLogicalId = logicalId
          /\ resetRelEpochSnapshot = relationshipEpoch
          /\ retiredStoreGeneration = fStoreGeneration
          THEN /\ staleOldOfferAccepted' = TRUE
               /\ staleOldOfferRejected' = staleOldOfferRejected
          ELSE /\ staleOldOfferAccepted' = FALSE
               /\ staleOldOfferRejected' = TRUE
    /\ UNCHANGED <<resetPhase, resetOp, historyEpoch,
                    resetHistorySnapshot, resetRelEpochSnapshot,
                    resetStoreGenerationSnapshot, replayedReset,
                    replayHistory, replayRelEpoch, replayStoreGeneration,
                    confirmAttempts, confirmDelivered, relationPhase,
                    logicalId, relationshipEpoch, fStoreGeneration,
                    retiredLogicalId, retiredStoreGeneration,
                    replacementKind, replacementStoreGeneration,
                    replacementUsed, committedLogicalId,
                    committedRelationshipEpoch, committedStoreGeneration,
                    siblingProgress>>

RestartFStore ==
    /\ relationPhase = "Live"
    /\ fStoreGeneration = 0
    /\ resetPhase \in {"Idle", "Confirmed"}
    /\ relationPhase' = "FRestarted"
    /\ retiredLogicalId' = logicalId
    /\ retiredStoreGeneration' = fStoreGeneration
    /\ fStoreGeneration' = fStoreGeneration + 1
    /\ UNCHANGED <<resetPhase, resetOp, historyEpoch,
                    resetHistorySnapshot, resetRelEpochSnapshot,
                    resetStoreGenerationSnapshot, replayedReset,
                    replayHistory, replayRelEpoch, replayStoreGeneration,
                    confirmAttempts, confirmDelivered, logicalId,
                    relationshipEpoch, replacementKind,
                    replacementStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

CreateAfterFRestart ==
    /\ relationPhase = "FRestarted"
    /\ relationshipEpoch < 2
    /\ relationPhase' = "RestartReplacementLive"
    /\ logicalId' = retiredLogicalId + 1
    /\ relationshipEpoch' = relationshipEpoch + 1
    /\ replacementKind' = "FRestart"
    /\ replacementStoreGeneration' = fStoreGeneration
    /\ historyEpoch' = 0
    /\ UNCHANGED <<resetPhase, resetOp, resetHistorySnapshot,
                    resetRelEpochSnapshot, resetStoreGenerationSnapshot,
                    replayedReset, replayHistory, replayRelEpoch,
                    replayStoreGeneration, confirmAttempts, confirmDelivered,
                    fStoreGeneration, retiredLogicalId,
                    retiredStoreGeneration, staleOldOfferAccepted,
                    staleOldOfferRejected, replacementUsed,
                    committedLogicalId, committedRelationshipEpoch,
                    committedStoreGeneration, siblingProgress>>

UseReplacement ==
    /\ relationPhase \in {"ReplacementLive", "RestartReplacementLive"}
    /\ ~replacementUsed
    /\ replacementUsed' = TRUE
    /\ committedLogicalId' = logicalId
    /\ committedRelationshipEpoch' = relationshipEpoch
    /\ committedStoreGeneration' = fStoreGeneration
    /\ UNCHANGED <<resetPhase, resetOp, historyEpoch,
                    resetHistorySnapshot, resetRelEpochSnapshot,
                    resetStoreGenerationSnapshot, replayedReset,
                    replayHistory, replayRelEpoch, replayStoreGeneration,
                    confirmAttempts, confirmDelivered, relationPhase,
                    logicalId, relationshipEpoch, fStoreGeneration,
                    retiredLogicalId, retiredStoreGeneration,
                    replacementKind, replacementStoreGeneration,
                    staleOldOfferAccepted, staleOldOfferRejected,
                    siblingProgress>>

ProgressSibling(r) ==
    /\ r \in SiblingLinks
    /\ r \notin siblingProgress
    /\ siblingProgress' = siblingProgress \cup {r}
    /\ UNCHANGED <<resetPhase, resetOp, historyEpoch,
                    resetHistorySnapshot, resetRelEpochSnapshot,
                    resetStoreGenerationSnapshot, replayedReset,
                    replayHistory, replayRelEpoch, replayStoreGeneration,
                    confirmAttempts, confirmDelivered, relationPhase,
                    logicalId, relationshipEpoch, fStoreGeneration,
                    retiredLogicalId, retiredStoreGeneration,
                    replacementKind, replacementStoreGeneration,
                    staleOldOfferAccepted, staleOldOfferRejected,
                    replacementUsed, committedLogicalId,
                    committedRelationshipEpoch, committedStoreGeneration>>

Next ==
    \/ RequestReset
    \/ ApplyReset
    \/ ObserveResetAck
    \/ SendResetConfirm
    \/ LoseResetConfirm
    \/ ReconnectAfterLostConfirm
    \/ ReplayPriorReset
    \/ ObserveReplayAck
    \/ ProcessResetConfirm
    \/ RetireSameFRelationship
    \/ CreateSameFReplacement
    \/ TryOldRelationshipOffer
    \/ RestartFStore
    \/ CreateAfterFRestart
    \/ UseReplacement
    \/ \E r \in SiblingLinks : ProgressSibling(r)
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ resetPhase \in ResetPhases
    /\ resetOp \in 0..1
    /\ historyEpoch \in 0..2
    /\ resetHistorySnapshot \in 0..2
    /\ resetRelEpochSnapshot \in 0..2
    /\ resetStoreGenerationSnapshot \in 0..1
    /\ replayedReset \in BOOLEAN
    /\ replayHistory \in 0..2
    /\ replayRelEpoch \in 0..2
    /\ replayStoreGeneration \in 0..1
    /\ confirmAttempts \in 0..2
    /\ confirmDelivered \in BOOLEAN
    /\ relationPhase \in RelationPhases
    /\ logicalId \in 0..2
    /\ relationshipEpoch \in 0..2
    /\ fStoreGeneration \in 0..2
    /\ retiredLogicalId \in 0..1
    /\ retiredStoreGeneration \in 0..1
    /\ replacementKind \in ReplacementKinds
    /\ replacementStoreGeneration \in 0..2
    /\ staleOldOfferAccepted \in BOOLEAN
    /\ staleOldOfferRejected \in BOOLEAN
    /\ replacementUsed \in BOOLEAN
    /\ committedLogicalId \in 0..2
    /\ committedRelationshipEpoch \in 0..2
    /\ committedStoreGeneration \in 0..2
    /\ siblingProgress \subseteq SiblingLinks

ResetReplayResultExact ==
    replayedReset =>
        /\ replayHistory = resetHistorySnapshot
        /\ replayRelEpoch = resetRelEpochSnapshot
        /\ replayStoreGeneration = resetStoreGenerationSnapshot

SameFReplacementPreservesStore ==
    replacementKind = "SameF" =>
        /\ fStoreGeneration = retiredStoreGeneration
        /\ replacementStoreGeneration = retiredStoreGeneration
        /\ relationshipEpoch > resetRelEpochSnapshot

FRestartReplacementChangesStore ==
    replacementKind = "FRestart" =>
        /\ fStoreGeneration > retiredStoreGeneration
        /\ replacementStoreGeneration = fStoreGeneration
        /\ relationshipEpoch > resetRelEpochSnapshot

OldRelationshipOfferRejected == ~staleOldOfferAccepted

ReplacementCommitUsesCurrentIdentity ==
    replacementUsed =>
        /\ relationPhase \in {"ReplacementLive", "RestartReplacementLive"}
        /\ committedLogicalId = logicalId
        /\ committedRelationshipEpoch = relationshipEpoch
        /\ committedStoreGeneration = fStoreGeneration

ReplacementUseWitness ==
    /\ replayedReset
    /\ confirmAttempts = 2
    /\ confirmDelivered
    /\ replacementKind = "SameF"
    /\ replacementUsed
    /\ staleOldOfferRejected
    /\ siblingProgress = SiblingLinks

ReplacementUseWitnessNotReached == ~ReplacementUseWitness

=============================================================================
