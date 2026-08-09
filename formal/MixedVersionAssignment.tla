---------------------- MODULE MixedVersionAssignment ----------------------
(***************************************************************************
A composed, two-epoch model of the Icecream p49/p50 assignment protocol.

Unlike AssignmentFenceCore, this module deliberately permits a delivered old
claim capability to outlive its assignment, restarts the scheduler/worker
session, and reuses the same 32-bit wire id for a new logical assignment.
It therefore checks the mixed-fleet theorem boundary directly:

  * TOKEN_REQUIRED on the new assignment rejects a delayed old legacy claim;
  * LEGACY_ID on the new assignment admits that claim, yielding the expected
    old-C arbitrary-delay ABA counterexample;
  * release is legal only after worker-side REVOKED (or an actual terminal);
  * CLAIMED but not STARTED remains revocable.

There is one worker and one shared wire id in the finite instance.  The two
logical assignments carry distinct epochs and capabilities by construction.
The implementation maps capabilities to nonzero OS-random 128-bit values;
the model represents freshness by distinct symbolic constants.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS OldAssignment, NewAssignment, NoAssignment,
          OldEpoch, NewEpoch,
          LegacyId, TokenRequired,
          NewAssignmentPolicy,
          MutantLegacyCanClaimToken,
          MutantReleaseBeforeRevoke

Assignments == {OldAssignment, NewAssignment}
Epochs == {OldEpoch, NewEpoch}
Policies == {LegacyId, TokenRequired}

ASSUME /\ OldAssignment # NewAssignment
       /\ NoAssignment \notin Assignments
       /\ OldEpoch # NewEpoch
       /\ LegacyId # TokenRequired
       /\ NewAssignmentPolicy \in Policies
       /\ MutantLegacyCanClaimToken \in BOOLEAN
       /\ MutantReleaseBeforeRevoke \in BOOLEAN

EpochOf(a) == IF a = OldAssignment THEN OldEpoch ELSE NewEpoch
PolicyOf(a) == IF a = OldAssignment THEN LegacyId ELSE NewAssignmentPolicy

Phases ==
    {"Unused", "Prepared", "Ready", "Delivered", "Claimed", "Started",
     "Revoked", "Terminal"}
RecordStates == {"NoRecord", "Reserved", "Claimed", "Started"}
RevokeResults == {"None", "Revoked", "Started"}

LiveSchedulerPhases ==
    {"Prepared", "Ready", "Delivered", "Claimed", "Started", "Revoked"}
LiveRecordStates == {"Reserved", "Claimed", "Started"}
ClaimEvidencePhases == {"Claimed", "Started", "Terminal"}

VARIABLES currentEpoch,
          workerEpoch,
          phase,
          schedulerReservation,
          workerSlot,
          recordAssignment,
          recordState,
          readyPending,
          revokeResult,
          deliveredEver,
          claimSource,
          claimHadToken,
          released,
          startCount,
          terminalCount

vars ==
    <<currentEpoch, workerEpoch, phase, schedulerReservation, workerSlot,
      recordAssignment, recordState, readyPending, revokeResult,
      deliveredEver, claimSource, claimHadToken, released, startCount,
      terminalCount>>

Init ==
    /\ currentEpoch = OldEpoch
    /\ workerEpoch = OldEpoch
    /\ phase = [a \in Assignments |-> "Unused"]
    /\ schedulerReservation = [a \in Assignments |-> FALSE]
    /\ workerSlot = [a \in Assignments |-> FALSE]
    /\ recordAssignment = NoAssignment
    /\ recordState = "NoRecord"
    /\ readyPending = [a \in Assignments |-> FALSE]
    /\ revokeResult = [a \in Assignments |-> "None"]
    /\ deliveredEver = [a \in Assignments |-> FALSE]
    /\ claimSource = [a \in Assignments |-> NoAssignment]
    /\ claimHadToken = [a \in Assignments |-> FALSE]
    /\ released = [a \in Assignments |-> FALSE]
    /\ startCount = [a \in Assignments |-> 0]
    /\ terminalCount = [a \in Assignments |-> 0]

(***************************************************************************
PREPARE atomically represents worker installation and queuing READY.  A real
implementation has send/receive microsteps, but the safety linearization point
is installation of the exact key on F.  No different live key may replace it.
***************************************************************************)
Prepare(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Unused"
    /\ EpochOf(a) = currentEpoch
    /\ workerEpoch = currentEpoch
    /\ recordAssignment = NoAssignment
    /\ phase' = [phase EXCEPT ![a] = "Prepared"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = TRUE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = TRUE]
    /\ recordAssignment' = a
    /\ recordState' = "Reserved"
    /\ readyPending' = [readyPending EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<currentEpoch, workerEpoch, revokeResult, deliveredEver,
                    claimSource, claimHadToken, released, startCount,
                    terminalCount>>

ConsumeReady(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Prepared"
    /\ recordAssignment = a
    /\ recordState = "Reserved"
    /\ readyPending[a]
    /\ phase' = [phase EXCEPT ![a] = "Ready"]
    /\ readyPending' = [readyPending EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<currentEpoch, workerEpoch, schedulerReservation,
                    workerSlot, recordAssignment, recordState, revokeResult,
                    deliveredEver, claimSource, claimHadToken, released,
                    startCount, terminalCount>>

DeliverUseCS(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Ready"
    /\ recordAssignment = a
    /\ recordState = "Reserved"
    /\ phase' = [phase EXCEPT ![a] = "Delivered"]
    /\ deliveredEver' = [deliveredEver EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<currentEpoch, workerEpoch, schedulerReservation,
                    workerSlot, recordAssignment, recordState, readyPending,
                    revokeResult, claimSource, claimHadToken, released,
                    startCount, terminalCount>>

(***************************************************************************
A delivered claim may arrive arbitrarily late.  Both assignments use the same
legacy wire id; PRESENTED identifies the logical capability that generated
the delayed claim.  A token-bearing claim always requires exact identity.  A
nonce-less claim is admitted only for LEGACY_ID, except in the named mutant.
***************************************************************************)
ClaimLegacy(presented) ==
    /\ presented \in Assignments
    /\ deliveredEver[presented]
    /\ recordAssignment # NoAssignment
    /\ LET current == recordAssignment
       IN /\ recordState = "Reserved"
          /\ phase[current] \in {"Prepared", "Ready", "Delivered"}
          /\ (PolicyOf(current) = LegacyId \/ MutantLegacyCanClaimToken)
          /\ phase' = [phase EXCEPT ![current] = "Claimed"]
          /\ recordState' = "Claimed"
          /\ claimSource' = [claimSource EXCEPT ![current] = presented]
          /\ claimHadToken' = [claimHadToken EXCEPT ![current] = FALSE]
    /\ UNCHANGED <<currentEpoch, workerEpoch, schedulerReservation,
                    workerSlot, recordAssignment, readyPending, revokeResult,
                    deliveredEver, released, startCount, terminalCount>>

ClaimToken(presented) ==
    /\ presented \in Assignments
    /\ deliveredEver[presented]
    /\ recordAssignment = presented
    /\ recordState = "Reserved"
    /\ phase[presented] \in {"Prepared", "Ready", "Delivered"}
    /\ phase' = [phase EXCEPT ![presented] = "Claimed"]
    /\ recordState' = "Claimed"
    /\ claimSource' = [claimSource EXCEPT ![presented] = presented]
    /\ claimHadToken' = [claimHadToken EXCEPT ![presented] = TRUE]
    /\ UNCHANGED <<currentEpoch, workerEpoch, schedulerReservation,
                    workerSlot, recordAssignment, readyPending, revokeResult,
                    deliveredEver, released, startCount, terminalCount>>

Start(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Claimed"
    /\ recordAssignment = a
    /\ recordState = "Claimed"
    /\ ~released[a]
    /\ phase' = [phase EXCEPT ![a] = "Started"]
    /\ recordState' = "Started"
    /\ startCount' = [startCount EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<currentEpoch, workerEpoch, schedulerReservation,
                    workerSlot, recordAssignment, readyPending, revokeResult,
                    deliveredEver, claimSource, claimHadToken, released,
                    terminalCount>>

(***************************************************************************
REVOKE wins from RESERVED and from CLAIMED-before-process-creation.  Once the
physical start action linearizes, the result is STARTED and ownership remains.
An absent record in ENFORCE is already default-deny and is idempotently
REVOKED; this finite core invokes RevokeNotStarted only for a live record.
***************************************************************************)
RevokeNotStarted(a) ==
    /\ a \in Assignments
    /\ recordAssignment = a
    /\ recordState \in {"Reserved", "Claimed"}
    /\ phase[a] \in {"Prepared", "Ready", "Delivered", "Claimed"}
    /\ phase' = [phase EXCEPT ![a] = "Revoked"]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ recordAssignment' = NoAssignment
    /\ recordState' = "NoRecord"
    /\ readyPending' = [readyPending EXCEPT ![a] = FALSE]
    /\ revokeResult' = [revokeResult EXCEPT ![a] = "Revoked"]
    /\ UNCHANGED <<currentEpoch, workerEpoch, schedulerReservation,
                    deliveredEver, claimSource, claimHadToken, released,
                    startCount, terminalCount>>

RevokeStarted(a) ==
    /\ a \in Assignments
    /\ recordAssignment = a
    /\ recordState = "Started"
    /\ phase[a] = "Started"
    /\ revokeResult' = [revokeResult EXCEPT ![a] = "Started"]
    /\ UNCHANGED <<currentEpoch, workerEpoch, phase,
                    schedulerReservation, workerSlot, recordAssignment,
                    recordState, readyPending, deliveredEver, claimSource,
                    claimHadToken, released, startCount, terminalCount>>

ConsumeRevoked(a) ==
    /\ a \in Assignments
    /\ (\/ /\ phase[a] = "Revoked"
            /\ revokeResult[a] = "Revoked"
        \/ /\ MutantReleaseBeforeRevoke
            /\ phase[a] \in {"Prepared", "Ready", "Delivered", "Claimed"})
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = FALSE]
    /\ released' = [released EXCEPT ![a] = TRUE]
    /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ revokeResult' = [revokeResult EXCEPT ![a] = "None"]
    /\ UNCHANGED <<currentEpoch, workerEpoch, workerSlot,
                    recordAssignment, recordState, readyPending,
                    deliveredEver, claimSource, claimHadToken, startCount>>

Complete(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Started"
    /\ recordAssignment = a
    /\ recordState = "Started"
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = FALSE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ recordAssignment' = NoAssignment
    /\ recordState' = "NoRecord"
    /\ released' = [released EXCEPT ![a] = TRUE]
    /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ revokeResult' = [revokeResult EXCEPT ![a] = "None"]
    /\ UNCHANGED <<currentEpoch, workerEpoch, readyPending, deliveredEver,
                    claimSource, claimHadToken, startCount>>

WorkerLost(a) ==
    /\ a \in Assignments
    /\ schedulerReservation[a]
    /\ recordAssignment = a
    /\ phase[a] \in {"Prepared", "Ready", "Delivered", "Claimed", "Started"}
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = FALSE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ recordAssignment' = NoAssignment
    /\ recordState' = "NoRecord"
    /\ readyPending' = [readyPending EXCEPT ![a] = FALSE]
    /\ revokeResult' = [revokeResult EXCEPT ![a] = "None"]
    /\ released' = [released EXCEPT ![a] = TRUE]
    /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<currentEpoch, workerEpoch, deliveredEver, claimSource,
                    claimHadToken, startCount>>

(***************************************************************************
Restart preserves DELIVEREDEVER: a suspended old client still holds its old
claim capability.  The old assignment must have settled before this compact
restart action; the full implementation also terminalizes all old-session
work as part of session loss.
***************************************************************************)
Restart ==
    /\ currentEpoch = OldEpoch
    /\ workerEpoch = OldEpoch
    /\ phase[OldAssignment] = "Terminal"
    /\ phase[NewAssignment] = "Unused"
    /\ recordAssignment = NoAssignment
    /\ currentEpoch' = NewEpoch
    /\ workerEpoch' = NewEpoch
    /\ UNCHANGED <<phase, schedulerReservation, workerSlot,
                    recordAssignment, recordState, readyPending, revokeResult,
                    deliveredEver, claimSource, claimHadToken, released,
                    startCount, terminalCount>>

Next ==
    \/ \E a \in Assignments : Prepare(a)
    \/ \E a \in Assignments : ConsumeReady(a)
    \/ \E a \in Assignments : DeliverUseCS(a)
    \/ \E p \in Assignments : ClaimLegacy(p)
    \/ \E p \in Assignments : ClaimToken(p)
    \/ \E a \in Assignments : Start(a)
    \/ \E a \in Assignments : RevokeNotStarted(a)
    \/ \E a \in Assignments : RevokeStarted(a)
    \/ \E a \in Assignments : ConsumeRevoked(a)
    \/ \E a \in Assignments : Complete(a)
    \/ \E a \in Assignments : WorkerLost(a)
    \/ Restart

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ currentEpoch \in Epochs
    /\ workerEpoch \in Epochs
    /\ phase \in [Assignments -> Phases]
    /\ schedulerReservation \in [Assignments -> BOOLEAN]
    /\ workerSlot \in [Assignments -> BOOLEAN]
    /\ recordAssignment \in Assignments \cup {NoAssignment}
    /\ recordState \in RecordStates
    /\ readyPending \in [Assignments -> BOOLEAN]
    /\ revokeResult \in [Assignments -> RevokeResults]
    /\ deliveredEver \in [Assignments -> BOOLEAN]
    /\ claimSource \in [Assignments -> (Assignments \cup {NoAssignment})]
    /\ claimHadToken \in [Assignments -> BOOLEAN]
    /\ released \in [Assignments -> BOOLEAN]
    /\ startCount \in [Assignments -> 0..1]
    /\ terminalCount \in [Assignments -> 0..1]

EpochCoherence ==
    /\ workerEpoch = currentEpoch
    /\ recordAssignment # NoAssignment
       => EpochOf(recordAssignment) = currentEpoch

RecordCoherence ==
    /\ (recordAssignment = NoAssignment) <=> (recordState = "NoRecord")
    /\ recordAssignment # NoAssignment
       => /\ recordState \in LiveRecordStates
          /\ workerSlot[recordAssignment]
          /\ phase[recordAssignment] \in
                {"Prepared", "Ready", "Delivered", "Claimed", "Started"}

ReservationCoherence ==
    \A a \in Assignments :
        schedulerReservation[a] <=> phase[a] \in LiveSchedulerPhases

WorkerSlotCoherence ==
    \A a \in Assignments :
        workerSlot[a] <=> (recordAssignment = a /\ recordState \in LiveRecordStates)

TerminalAtMostOnce ==
    \A a \in Assignments : terminalCount[a] <= 1

TerminalCoherence ==
    \A a \in Assignments :
        (phase[a] = "Terminal") <=> (terminalCount[a] = 1)

ReleaseSafety ==
    \A a \in Assignments :
        released[a]
        => /\ phase[a] = "Terminal"
           /\ ~schedulerReservation[a]
           /\ ~workerSlot[a]
           /\ recordAssignment # a

NoUnauthorizedStart ==
    \A a \in Assignments :
        startCount[a] > 0 => claimSource[a] = a

TokenRequiredExactness ==
    \A a \in Assignments :
        PolicyOf(a) = TokenRequired /\ phase[a] \in ClaimEvidencePhases
        => /\ claimHadToken[a]
           /\ claimSource[a] = a

LegacyClaimPolicy ==
    \A a \in Assignments :
        phase[a] \in ClaimEvidencePhases /\ ~claimHadToken[a]
        => PolicyOf(a) = LegacyId

SafetyInvariant ==
    /\ TypeOK
    /\ EpochCoherence
    /\ RecordCoherence
    /\ ReservationCoherence
    /\ WorkerSlotCoherence
    /\ TerminalAtMostOnce
    /\ TerminalCoherence
    /\ ReleaseSafety
    /\ NoUnauthorizedStart
    /\ TokenRequiredExactness
    /\ LegacyClaimPolicy

=============================================================================
