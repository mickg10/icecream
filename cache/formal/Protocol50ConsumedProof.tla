------------------------- MODULE Protocol50ConsumedProof -------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Focused service-side refinement model for the consumed-binding proof carried
from an interrupted old physical link through RECOVER validation and RESET.

The target and one colliding sibling each have a reservation at prepared
ordinal 2, physical generation 1, after a committed prefix K=1. Their stable
reservation, C/F, and logical-relationship identities differ. All configured
CxF links are represented, so the six topology configurations exercise both
multi-C/single-F and single-C/multi-F directions; all non-target links can
make independent progress. The service reservation map is F-owner-local:
the wrong-row selector mutant may collide across C relationships on the same
F, but never scans or mutates a row owned by another F.

Interrupted settlement clears only the relationship's pending ordinal and
generation. The surviving row's consumed binding remains exact until RECOVER
checks it; the RESET commit then clears that consumed proof and re-arms its
credit once. A lost RESET reply is retried from cached state without a second
credit transition. This is a bounded service-ownership refinement witness,
not a wire codec, worker-memory, scheduler, or full pipeline safety model.
***************************************************************************)

CONSTANTS CStores, FStores, TargetC, TargetF, DecoyC, DecoyF,
          MutantClearConsumedProof,
          MutantCrossRelationshipSelection,
          MutantDoubleRearmOnRetry

ASSUME /\ CStores # {}
       /\ FStores # {}
       /\ TargetC \in CStores
       /\ TargetF \in FStores
       /\ DecoyC \in CStores
       /\ DecoyF \in FStores
       /\ <<TargetC, TargetF>> # <<DecoyC, DecoyF>>
       /\ MutantClearConsumedProof \in BOOLEAN
       /\ MutantCrossRelationshipSelection \in BOOLEAN
       /\ MutantDoubleRearmOnRetry \in BOOLEAN

Links == CStores \X FStores
TargetLink == <<TargetC, TargetF>>
DecoyLink == <<DecoyC, DecoyF>>
Rows == {"target", "decoy"}
TargetRow == "target"
DecoyRow == "decoy"
NoRow == "none"
NoBinding == [reservationId |-> 0, cStore |-> 0, fStore |-> 0,
              logicalId |-> 0, relationshipEpoch |-> 0,
              relationshipOrdinal |-> 0, physicalGeneration |-> 0]

LinkOf(row) == IF row = TargetRow THEN TargetLink ELSE DecoyLink
ReservationId(row) == IF row = TargetRow THEN 101 ELSE 202
LogicalId(row) == IF row = TargetRow THEN 11 ELSE 22
Ordinal == 2
CommittedPrefix == 1
OldPhysicalGeneration == 1
NewPhysicalGeneration == 2

Binding(row, epoch, generation) ==
    [reservationId |-> ReservationId(row),
     cStore |-> LinkOf(row)[1],
     fStore |-> LinkOf(row)[2],
     logicalId |-> LogicalId(row),
     relationshipEpoch |-> epoch,
     relationshipOrdinal |-> Ordinal,
     physicalGeneration |-> generation]

AllBindings == {NoBinding} \cup
    {Binding(row, epoch, generation) :
        row \in Rows, epoch \in 1..2, generation \in 1..2}
RowStates == {"Available", "Consumed", "CancelRequested", "Cancelled",
              "Rearmed"}
AckStates == {"None", "Pending", "Lost", "Replayed", "Confirmed"}

VARIABLES currentGeneration, currentEpoch, committedPrefix,
          pendingRow, pendingOrdinal, pendingGeneration, rowState, proof,
          outstanding, rearmCount, settled, recoveryAccepted, resetApplied,
          resetAck, cachedAckEpoch, siblingProgress

vars == <<currentGeneration, currentEpoch, committedPrefix, pendingRow,
          pendingOrdinal, pendingGeneration, rowState, proof, outstanding,
          rearmCount, settled, recoveryAccepted, resetApplied, resetAck,
          cachedAckEpoch, siblingProgress>>

Init ==
    /\ currentGeneration = [r \in Links |-> OldPhysicalGeneration]
    /\ currentEpoch = [r \in Links |-> 1]
    /\ committedPrefix = [r \in Links |-> 0]
    /\ pendingRow = [r \in Links |-> NoRow]
    /\ pendingOrdinal = [r \in Links |-> 0]
    /\ pendingGeneration = [r \in Links |-> 0]
    /\ rowState = [row \in Rows |-> "Available"]
    /\ proof = [row \in Rows |-> NoBinding]
    /\ outstanding = [r \in Links |->
          IF r \in {TargetLink, DecoyLink} THEN 1 ELSE 0]
    /\ rearmCount = [r \in Links |-> 0]
    /\ settled = {}
    /\ recoveryAccepted = {}
    /\ resetApplied = {}
    /\ resetAck = [r \in Links |-> "None"]
    /\ cachedAckEpoch = [r \in Links |-> 0]
    /\ siblingProgress = {}

TypeOK ==
    /\ currentGeneration \in [Links -> {OldPhysicalGeneration,
                                            NewPhysicalGeneration}]
    /\ currentEpoch \in [Links -> 1..2]
    /\ committedPrefix \in [Links -> 0..CommittedPrefix]
    /\ pendingRow \in [Links -> Rows \cup {NoRow}]
    /\ pendingOrdinal \in [Links -> 0..Ordinal]
    /\ pendingGeneration \in [Links -> 0..NewPhysicalGeneration]
    /\ rowState \in [Rows -> RowStates]
    /\ proof \in [Rows -> AllBindings]
    /\ outstanding \in [Links -> 0..2]
    /\ rearmCount \in [Links -> 0..2]
    /\ settled \subseteq {TargetLink}
    /\ recoveryAccepted \subseteq {TargetLink}
    /\ resetApplied \subseteq {TargetLink}
    /\ resetAck \in [Links -> AckStates]
    /\ cachedAckEpoch \in [Links -> 0..2]
    /\ siblingProgress \subseteq Links \ {TargetLink}

CommitPrefix(r) ==
    /\ r \in {TargetLink, DecoyLink}
    /\ committedPrefix[r] = 0
    /\ committedPrefix' = [committedPrefix EXCEPT ![r] = CommittedPrefix]
    /\ UNCHANGED <<currentGeneration, currentEpoch, pendingRow,
                    pendingOrdinal, pendingGeneration, rowState, proof,
                    outstanding, rearmCount, settled, recoveryAccepted,
                    resetApplied, resetAck, cachedAckEpoch, siblingProgress>>

Consume(row) ==
    LET r == LinkOf(row)
    IN /\ row \in Rows
       /\ rowState[row] \in {"Available", "Rearmed"}
       /\ committedPrefix[r] = CommittedPrefix
       /\ pendingRow[r] = NoRow
       /\ proof[row] = NoBinding
       /\ outstanding[r] = 1
       /\ pendingRow' = [pendingRow EXCEPT ![r] = row]
       /\ pendingOrdinal' = [pendingOrdinal EXCEPT ![r] = Ordinal]
       /\ pendingGeneration' = [pendingGeneration EXCEPT
                                  ![r] = currentGeneration[r]]
       /\ rowState' = [rowState EXCEPT ![row] = "Consumed"]
       /\ proof' = [proof EXCEPT ![row] =
             Binding(row, currentEpoch[r], currentGeneration[r])]
       /\ outstanding' = [outstanding EXCEPT ![r] = @ - 1]
       /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                       rearmCount, settled, recoveryAccepted, resetApplied,
                       resetAck, cachedAckEpoch, siblingProgress>>

CancelDecoy ==
    /\ rowState[DecoyRow] = "Consumed"
    /\ proof[DecoyRow] = Binding(DecoyRow, 1, OldPhysicalGeneration)
    /\ rowState' = [rowState EXCEPT ![DecoyRow] = "CancelRequested"]
    /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                    pendingRow, pendingOrdinal, pendingGeneration, proof,
                    outstanding, rearmCount, settled, recoveryAccepted,
                    resetApplied, resetAck, cachedAckEpoch, siblingProgress>>

SettleInterruptedTarget ==
    /\ pendingRow[TargetLink] = TargetRow
    /\ pendingOrdinal[TargetLink] = Ordinal
    /\ pendingGeneration[TargetLink] = OldPhysicalGeneration
    /\ currentGeneration[TargetLink] = OldPhysicalGeneration
    /\ rowState[TargetRow] = "Consumed"
    /\ proof[TargetRow] = Binding(TargetRow, 1, OldPhysicalGeneration)
    /\ currentGeneration' = [currentGeneration EXCEPT
                              ![TargetLink] = NewPhysicalGeneration]
    /\ pendingRow' = [pendingRow EXCEPT ![TargetLink] = NoRow]
    /\ pendingOrdinal' = [pendingOrdinal EXCEPT ![TargetLink] = 0]
    /\ pendingGeneration' = [pendingGeneration EXCEPT ![TargetLink] = 0]
    /\ proof' = [proof EXCEPT
          ![TargetRow] = IF MutantClearConsumedProof
                         THEN NoBinding ELSE @,
          ![DecoyRow] = IF MutantCrossRelationshipSelection
                            /\ LinkOf(DecoyRow)[2] = TargetF
                            /\ rowState[DecoyRow] = "CancelRequested"
                            /\ proof[DecoyRow] =
                                  Binding(DecoyRow, 1, OldPhysicalGeneration)
                            /\ pendingOrdinal[DecoyLink] = Ordinal
                            /\ pendingGeneration[DecoyLink] =
                                  OldPhysicalGeneration
                         THEN NoBinding ELSE @]
    /\ rowState' = [rowState EXCEPT
          ![DecoyRow] = IF MutantCrossRelationshipSelection
                            /\ LinkOf(DecoyRow)[2] = TargetF
                            /\ rowState[DecoyRow] = "CancelRequested"
                            /\ proof[DecoyRow] =
                                  Binding(DecoyRow, 1, OldPhysicalGeneration)
                            /\ pendingOrdinal[DecoyLink] = Ordinal
                            /\ pendingGeneration[DecoyLink] =
                                  OldPhysicalGeneration
                         THEN "Cancelled" ELSE @]
    /\ settled' = settled \cup {TargetLink}
    /\ UNCHANGED <<currentEpoch, committedPrefix, outstanding, rearmCount,
                    recoveryAccepted, resetApplied, resetAck, cachedAckEpoch,
                    siblingProgress>>

RecoverTarget ==
    /\ TargetLink \in settled
    /\ currentGeneration[TargetLink] = NewPhysicalGeneration
    /\ currentEpoch[TargetLink] = 1
    /\ committedPrefix[TargetLink] = CommittedPrefix
    /\ pendingRow[TargetLink] = NoRow
    /\ pendingOrdinal[TargetLink] = 0
    /\ pendingGeneration[TargetLink] = 0
    /\ rowState[TargetRow] = "Consumed"
    /\ proof[TargetRow] = Binding(TargetRow, 1, OldPhysicalGeneration)
    /\ Binding(TargetRow, 1, OldPhysicalGeneration).cStore = TargetC
    /\ Binding(TargetRow, 1, OldPhysicalGeneration).fStore = TargetF
    /\ Binding(TargetRow, 1, OldPhysicalGeneration).logicalId =
          LogicalId(TargetRow)
    /\ Binding(TargetRow, 1, OldPhysicalGeneration).relationshipEpoch =
          currentEpoch[TargetLink]
    /\ Binding(TargetRow, 1, OldPhysicalGeneration).relationshipOrdinal =
          committedPrefix[TargetLink] + 1
    /\ Binding(TargetRow, 1, OldPhysicalGeneration).physicalGeneration <
          currentGeneration[TargetLink]
    /\ recoveryAccepted' = recoveryAccepted \cup {TargetLink}
    /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                    pendingRow, pendingOrdinal, pendingGeneration, rowState,
                    proof, outstanding, rearmCount, settled, resetApplied,
                    resetAck, cachedAckEpoch, siblingProgress>>

ApplyResetCommit ==
    /\ TargetLink \in recoveryAccepted
    /\ TargetLink \notin resetApplied
    /\ rowState[TargetRow] = "Consumed"
    /\ proof[TargetRow] = Binding(TargetRow, 1, OldPhysicalGeneration)
    /\ outstanding[TargetLink] = 0
    /\ committedPrefix[TargetLink] = CommittedPrefix
    /\ currentEpoch' = [currentEpoch EXCEPT ![TargetLink] = @ + 1]
    /\ proof' = [proof EXCEPT ![TargetRow] = NoBinding]
    /\ rowState' = [rowState EXCEPT ![TargetRow] = "Rearmed"]
    /\ outstanding' = [outstanding EXCEPT ![TargetLink] = @ + 1]
    /\ rearmCount' = [rearmCount EXCEPT ![TargetLink] = @ + 1]
    /\ resetApplied' = resetApplied \cup {TargetLink}
    /\ resetAck' = [resetAck EXCEPT ![TargetLink] = "Pending"]
    /\ cachedAckEpoch' = [cachedAckEpoch EXCEPT
                            ![TargetLink] = currentEpoch[TargetLink] + 1]
    /\ UNCHANGED <<currentGeneration, committedPrefix, pendingRow,
                    pendingOrdinal, pendingGeneration, settled,
                    recoveryAccepted, siblingProgress>>

LoseResetAck ==
    /\ resetAck[TargetLink] = "Pending"
    /\ resetAck' = [resetAck EXCEPT ![TargetLink] = "Lost"]
    /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                    pendingRow, pendingOrdinal, pendingGeneration, rowState,
                    proof, outstanding, rearmCount, settled, recoveryAccepted,
                    resetApplied, cachedAckEpoch, siblingProgress>>

RetryCachedResetAck ==
    /\ resetAck[TargetLink] = "Lost"
    /\ TargetLink \in resetApplied
    /\ resetAck' = [resetAck EXCEPT ![TargetLink] = "Replayed"]
    /\ IF MutantDoubleRearmOnRetry
          THEN /\ outstanding' = [outstanding EXCEPT ![TargetLink] = @ + 1]
               /\ rearmCount' = [rearmCount EXCEPT ![TargetLink] = @ + 1]
          ELSE /\ UNCHANGED outstanding
               /\ UNCHANGED rearmCount
    /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                    pendingRow, pendingOrdinal, pendingGeneration, rowState,
                    proof, settled, recoveryAccepted, resetApplied,
                    cachedAckEpoch, siblingProgress>>

ConfirmReset ==
    /\ resetAck[TargetLink] \in {"Pending", "Replayed"}
    /\ resetAck' = [resetAck EXCEPT ![TargetLink] = "Confirmed"]
    /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                    pendingRow, pendingOrdinal, pendingGeneration, rowState,
                    proof, outstanding, rearmCount, settled, recoveryAccepted,
                    resetApplied, cachedAckEpoch, siblingProgress>>

ProgressSibling(r) ==
    /\ r \in Links \ {TargetLink}
    /\ r \notin siblingProgress
    /\ siblingProgress' = siblingProgress \cup {r}
    /\ UNCHANGED <<currentGeneration, currentEpoch, committedPrefix,
                    pendingRow, pendingOrdinal, pendingGeneration, rowState,
                    proof, outstanding, rearmCount, settled, recoveryAccepted,
                    resetApplied, resetAck, cachedAckEpoch>>

Next ==
    \/ \E r \in {TargetLink, DecoyLink} : CommitPrefix(r)
    \/ \E row \in Rows : Consume(row)
    \/ CancelDecoy
    \/ SettleInterruptedTarget
    \/ RecoverTarget
    \/ ApplyResetCommit
    \/ LoseResetAck
    \/ RetryCachedResetAck
    \/ ConfirmReset
    \/ \E r \in Links : ProgressSibling(r)
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

ConsumedProofMatchesExactOwner ==
    \A row \in Rows : proof[row] # NoBinding =>
        /\ proof[row].reservationId = ReservationId(row)
        /\ proof[row].cStore = LinkOf(row)[1]
        /\ proof[row].fStore = LinkOf(row)[2]
        /\ proof[row].logicalId = LogicalId(row)
        /\ proof[row].relationshipEpoch \in 1..2
        /\ proof[row].relationshipOrdinal = Ordinal
        /\ proof[row].physicalGeneration \in {OldPhysicalGeneration,
                                                  NewPhysicalGeneration}

EveryConsumedReservationRetainsItsProof ==
    \A row \in Rows : rowState[row] \in {"Consumed", "CancelRequested"}
        => proof[row] # NoBinding

SurvivingProofRetainedThroughRecover ==
    TargetLink \in settled /\ TargetLink \notin resetApplied =>
        /\ rowState[TargetRow] = "Consumed"
        /\ proof[TargetRow] = Binding(TargetRow, 1, OldPhysicalGeneration)

OtherRelationshipUntouchedByTargetSettlement ==
    TargetLink \in settled /\
        rowState[DecoyRow] \in {"Consumed", "CancelRequested", "Cancelled"}
        => /\ rowState[DecoyRow] # "Cancelled"
           /\ proof[DecoyRow] = Binding(DecoyRow, 1, OldPhysicalGeneration)

RecoveryRequiresExactConsumedProof ==
    TargetLink \in recoveryAccepted /\ TargetLink \notin resetApplied =>
        /\ rowState[TargetRow] = "Consumed"
        /\ proof[TargetRow] = Binding(TargetRow, 1, OldPhysicalGeneration)

ResetOnlyAfterVerifiedRecovery ==
    TargetLink \in resetApplied => TargetLink \in recoveryAccepted

ResetCreditRearmedAtMostOnce ==
    /\ \A r \in Links : rearmCount[r] <= 1
    /\ \A r \in Links : outstanding[r] <= 1

ResetRetryUsesCachedEpoch ==
    resetAck[TargetLink] \in {"Replayed", "Confirmed"} =>
        /\ cachedAckEpoch[TargetLink] = currentEpoch[TargetLink]
        /\ rearmCount[TargetLink] = 1

CommittedPrefixSurvivesReset ==
    TargetLink \in resetApplied =>
        committedPrefix[TargetLink] = CommittedPrefix

RearmedReservationGetsFreshEpochBinding ==
    rowState[TargetRow] = "Consumed" /\ currentEpoch[TargetLink] = 2 =>
        /\ proof[TargetRow] = Binding(TargetRow, 2, NewPhysicalGeneration)
        /\ proof[TargetRow] # Binding(TargetRow, 1, OldPhysicalGeneration)

ConsumedProofResetWitness ==
    /\ TargetLink \in settled
    /\ TargetLink \in recoveryAccepted
    /\ TargetLink \in resetApplied
    /\ resetAck[TargetLink] = "Confirmed"
    /\ currentGeneration[TargetLink] = NewPhysicalGeneration
    /\ currentEpoch[TargetLink] = 2
    /\ rowState[TargetRow] = "Rearmed"
    /\ proof[TargetRow] = NoBinding
    /\ outstanding[TargetLink] = 1
    /\ rearmCount[TargetLink] = 1
    /\ cachedAckEpoch[TargetLink] = 2
    /\ committedPrefix[TargetLink] = CommittedPrefix
    /\ rowState[DecoyRow] = "CancelRequested"
    /\ proof[DecoyRow] = Binding(DecoyRow, 1, OldPhysicalGeneration)
    /\ siblingProgress = Links \ {TargetLink}

ConsumedProofResetWitnessNotReached == ~ConsumedProofResetWitness

=============================================================================
